#include "native/bridge_contracts.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t kInit = 0x08879DA4u, kReset = 0x08879D58u;
constexpr std::uint32_t kAlloc = 0x08879DB4u, kReverse = 0x08879F08u, kFree = 0x08879FF0u;
constexpr std::uint32_t kManager = 0x08200000u, kHeap = 0x09000000u, kHeapBytes = 0x10000u;
constexpr std::uint32_t kSource = 0x09200000u, kOwner = 0x08300000u, kStack = 0x08400080u;
constexpr std::uint32_t kGlobalManager = 0x09FBE75Cu, kReturn = 0x08001000u;
constexpr std::uint32_t kCaller = 0x088B03B0u, kBuilder = 0x0889E5C0u;
constexpr std::string_view kElfHash = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
void require(bool value, const std::string &reason) { if (!value) throw std::runtime_error(reason); }
void put(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    require(at <= bytes.size() && bytes.size() - at >= 4u, "Fixture word out of range");
    for (unsigned i = 0; i < 4u; ++i) bytes[at+i] = static_cast<std::uint8_t>(value >> (8u*i));
}

// Constructed parent with an indexed child at slot two. Its source extent is
// known by construction; this does not stand in for the live virtual provider.
std::vector<std::uint8_t> parent_fixture(std::uint32_t records) {
    std::vector<std::uint8_t> bytes(48u + records * 40u, 0u);
    put(bytes, 0u, 3u); put(bytes, 20u, 32u); put(bytes, 24u, 16u + records * 40u);
    put(bytes, 40u, records);
    for (std::uint32_t i = 0; i < records; ++i) {
        const auto record = 48u + i * 40u;
        put(bytes, record, 40u); put(bytes, record+8u, 1u);
        put(bytes, record+16u, 24u); put(bytes, record+24u, 8u);
        put(bytes, record+28u, 4u | (4u << 16u));
    }
    return bytes;
}

class Gate {
    Runtime aot_, interpreter_;
    std::mt19937 random_{0x414C4C43u};
    std::vector<std::uint8_t> baseline_;
    std::uint32_t mirror_{};
public:
    std::uint64_t calls{}, allocations{}, frees{}, resets{}, caller_cases{}, reused_addresses{};
    unsigned max_slices{}, reverse_alignment_residues{}, virtual_provider_cases{};
    explicit Gate(const Elf32Image &elf) : aot_(elf.required_ram_size()), interpreter_(elf.required_ram_size()) {
        (void)elf.load_and_relocate(aot_.memory());
        (void)elf.load_and_relocate(interpreter_.memory());
        register_generated_functions(aot_);
        require(mhp3rd::native::matches_code_fingerprint<940>(aot_.memory(), kReset,
            "f3a3ff77bea6d041cc38be9e261f64d61bb02ce1faaf215a93acaaaefcf86047"), "Heap family changed");
        require(mhp3rd::native::matches_code_fingerprint<132>(aot_.memory(), kCaller,
            "53fbebd4e4702ab07b244cb99064a6dc36b79d553964729e9d6d0fe5a45e8d60"), "Caller tail changed");
        require(mhp3rd::native::matches_code_fingerprint<156>(aot_.memory(), 0x088B0398u,
            "f845198f44ce90860a0c462535039d0efbb195a31c33f19c16b18eff3dc36cc6"), "Virtual caller segment changed");
        require(mhp3rd::native::matches_code_fingerprint<36>(aot_.memory(), 0x088B7DE0u,
            "d17da887d7cd999fae774f22c747885ed2ef90be06cf0bd3909bf9f34d5e7fdd"), "Virtual slot provider changed");
        require(mhp3rd::native::matches_code_fingerprint<36>(aot_.memory(), 0x089E8834u,
            "6b3630c3cd2c5e41756d6048bef85abe930f7ddf93796b5a2bb9f7222b98cba7"), "Slot offsets changed");
        require(mhp3rd::native::matches_code_fingerprint<36>(aot_.memory(), 0x089E8810u,
            "1d921ef9af87716c9b6dcc60a338ca3a14b05edae91c46e8e8c3c132bf163b2a"), "Slot capacities changed");
        require(aot_.memory().load32(0x0896FBC8u+120u)==0x088B7DE0u &&
                aot_.memory().load32(0x089E8834u+28u)==0x26800u &&
                aot_.memory().load32(0x089E8810u+28u)==0x5800u,
                "Selected virtual source binding changed");
        baseline_ = aot_.memory().bytes();
    }
    std::uint32_t address(std::uint32_t value) const { return value | mirror_; }
    const GuestMemory &memory() const { return aot_.memory(); }
    void word(std::uint32_t address, std::uint32_t value) {
        aot_.memory().store32(address, value); interpreter_.memory().store32(address, value);
    }
    void copy(std::uint32_t address, std::span<const std::uint8_t> bytes) {
        aot_.memory().copy_in(address, bytes); interpreter_.memory().copy_in(address, bytes);
    }
    AllegrexContext context(std::uint32_t entry) {
        AllegrexContext ctx{};
        for (auto &value : ctx.gpr) value = random_();
        for (auto &value : ctx.fpr) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &value : ctx.vfpu) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &value : ctx.vfpu_ctrl) value = random_();
        ctx.hi = random_(); ctx.lo = random_(); ctx.fcr31 = random_();
        ctx.gpr[0] = 0u; ctx.gpr[29] = address(kStack); ctx.gpr[31] = kReturn; ctx.pc = entry;
        return ctx;
    }
    AllegrexContext run(AllegrexContext ctx) {
        auto interpreted = ctx;
        require(aot_.invoke_isolated_aot(ctx.pc, ctx) && !aot_.stopped() && ctx.pc == kReturn,
                "Original AOT did not return");
        unsigned slices = 0u;
        while (interpreted.pc != kReturn && slices++ < 100000u) {
            const auto pc = interpreted.pc;
            const bool allowed = (pc >= kReset && pc < 0x0887A104u) ||
                (pc >= 0x088B0398u && pc < 0x088B0434u) ||
                (pc >= 0x088B7DE0u && pc < 0x088B7E04u) ||
                (pc >= 0x088661BCu && pc < 0x08866234u) ||
                (pc >= kBuilder && pc < kBuilder+528u) ||
                (pc >= 0x08876A10u && pc < 0x08876AFCu);
            require(allowed, "Interpreter escaped certified spans at " + std::to_string(pc));
            require(interpret_allegrex(interpreter_, interpreted, 1u) == InterpreterExit::Budget &&
                    !interpreter_.stopped(), "Original interpreter failed");
        }
        require(interpreted.pc == kReturn, "Original call exceeded instruction budget");
        require(mhp3rd::native::same_context(ctx, interpreted), "AOT/interpreter context differs");
        require(aot_.memory().bytes() == interpreter_.memory().bytes(), "AOT/interpreter RAM differs");
        require(aot_.memory().vram_bytes() == interpreter_.memory().vram_bytes(), "AOT/interpreter VRAM differs");
        max_slices = std::max(max_slices, slices); ++calls;
        return ctx;
    }
    void initialize(std::uint32_t size = kHeapBytes, std::uint32_t mirror = 0u) {
        mirror_ = mirror;
        copy(GuestMemory::kPhysicalBase, baseline_);
        auto ctx = context(kInit);
        ctx.gpr[4] = address(kManager); ctx.gpr[5] = address(kHeap); ctx.gpr[6] = size;
        (void)run(ctx);
        require(memory().load32(kManager) == size && memory().load32(kManager+4u) == address(kHeap) &&
                memory().load32(kManager+16u) == address(kHeap), "Initializer owner fields differ");
        require(memory().load32(kHeap+16u) == 2u && memory().load32(kHeap+20u) == (size-32u)/16u,
                "Initializer free capacity differs");
        ++resets;
    }
    std::uint32_t allocate(std::uint32_t size, std::uint32_t alignment, bool reverse = false) {
        auto ctx = context(reverse ? kReverse : kAlloc);
        ctx.gpr[4] = address(kManager); ctx.gpr[5] = size; ctx.gpr[6] = alignment;
        const auto returned = run(ctx).gpr[2]; ++allocations;
        if (returned != 0u) {
            const auto p = GuestMemory::canonical(returned);
            require(p >= kHeap+32u && std::uint64_t(p)+size <= std::uint64_t(kHeap)+memory().load32(kManager),
                    "Requested payload exceeds owned arena");
            if (reverse && (returned & (alignment-1u)) != 0u) ++reverse_alignment_residues;
            require((returned & ((reverse ? 16u : alignment)-1u)) == 0u,
                    "Forward alignment or reverse 16-byte alignment differs");
            require(memory().load32(returned-28u) != 0u, "New node has no live predecessor");
        }
        return returned;
    }
    void release(std::uint32_t pointer) {
        auto ctx = context(kFree); ctx.gpr[4] = address(kManager); ctx.gpr[5] = pointer;
        (void)run(ctx); ++frees;
        if (pointer) require(memory().load32(pointer-28u) == 0u, "Freed node remains linked backward");
    }
    void reset_heap() {
        auto ctx = context(kReset); ctx.gpr[4] = address(kManager); (void)run(ctx); ++resets;
        require(memory().load32(kHeap) == 0u && memory().load32(kHeap+20u) == (memory().load32(kManager)-32u)/16u,
                "Reset did not recover root capacity");
    }
    void lifecycle(std::uint32_t mirror) {
        initialize(kHeapBytes, mirror);
        const auto before = memory().bytes();
        require(allocate(0u,16u) == 0u && memory().bytes() == before, "Zero allocation changed RAM");
        const auto first = allocate(108u,16u), second = allocate(72u,16u), third = allocate(36u,16u);
        require(first && second && third, "Small allocation failed");
        release(second);
        const auto replacement = allocate(72u,16u);
        require(replacement == second, "Expected first-fit address reuse not observed"); ++reused_addresses;
        release(third); release(replacement); release(first);
        require(memory().load32(kHeap+20u) == (kHeapBytes-32u)/16u, "Freed blocks did not coalesce");
        release(0u);
        const auto before_reset = allocate(36u,16u); reset_heap();
        require(allocate(36u,16u) == before_reset, "Reset address reuse not observed"); ++reused_addresses;
        reset_heap();
        struct Allocation { std::uint32_t pointer{}, bytes{}; };
        std::vector<Allocation> live;
        for (unsigned i = 0; i < 24u; ++i) {
            const auto bytes = 36u * (1u + (i*17u)%75u), alignment = 16u << (i%3u);
            const auto p = allocate(bytes,alignment,(i&1u)!=0u);
            require(p != 0u, "Bounded mixed allocation failed");
            for (const auto other : live) {
                const auto a = std::uint64_t(GuestMemory::canonical(p));
                const auto b = std::uint64_t(GuestMemory::canonical(other.pointer));
                require(a+bytes <= b || b+other.bytes <= a, "Live requested payloads overlap");
            }
            live.push_back({p,bytes});
        }
        for (std::size_t i = 0; i < live.size(); i+=2u) release(live[i].pointer);
        for (std::size_t i = 1; i < live.size(); i+=2u) release(live[i].pointer);
        require(memory().load32(kHeap+20u) == (kHeapBytes-32u)/16u, "Mixed frees did not coalesce");
        initialize(256u,mirror);
        const auto only = allocate(108u,16u); require(only != 0u, "Tiny arena first allocation failed");
        const auto exhausted = memory().bytes();
        require(allocate(108u,16u)==0u && memory().bytes()==exhausted, "Exhaustion changed RAM or succeeded");
        release(only); require(allocate(108u,16u)==only, "Exhausted arena failed reuse"); ++reused_addresses;
    }
    void caller(std::uint32_t count, std::uint32_t mirror, bool virtual_provider) {
        initialize(kHeapBytes,mirror);
        const auto source_base = virtual_provider ? kOwner+0x27C70u : kSource;
        const auto source = parent_fixture(count); copy(source_base,source);
        word(kOwner,0x0896FBC8u);
        require(source.size()<=0x5800u,"Constructed root exceeds source slot");
        word(kGlobalManager,address(kManager)); word(kStack+116u,kReturn);
        auto ctx = context(virtual_provider ? 0x088B0398u : kCaller);
        ctx.gpr[2] = address(source_base); ctx.gpr[30] = address(kOwner);
        const auto returned = run(ctx);
        require(returned.gpr[2] == 1u && returned.gpr[29] == address(kStack)+128u,
                "Caller epilogue differs");
        const auto state = address(kOwner+5104u);
        require(memory().load32(state) == address(source_base+32u) && memory().load8(state+8u)==count,
                "Caller did not bind indexed child two");
        const auto command = memory().load32(state+4u);
        require(command != 0u && std::uint64_t(GuestMemory::canonical(command))+count*36u <= kHeap+kHeapBytes,
                "Caller command allocation missing or outside arena");
        for (std::uint32_t i=0; i<count; ++i) {
            const auto image = address(source_base+80u+i*40u);
            require(memory().load32(command+i*36u)==0xC2000000u &&
                    memory().load32(command+i*36u+8u)==(0xA0000000u|(image&0xFFFFFFu)) &&
                    memory().load32(command+i*36u+32u)==0xC4000000u,
                    "Caller commands differ from constructed source");
        }
        std::vector<std::uint8_t> after(source.size()); memory().copy_out(source_base,after);
        require(after==source,"Caller changed source root");
        ++caller_cases;
        if (virtual_provider) ++virtual_provider_cases;
    }
};
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc==3,"usage: texture_allocation_oracle EBOOT.ELF new-report.json");
        require(!std::filesystem::exists(argv[2]) && !std::filesystem::is_symlink(argv[2]),"Report already exists");
        require(sha256_file(argv[1])==kElfHash,"Unsupported ELF");
        Gate gate(Elf32Image::from_file(argv[1]));
        for (const auto mirror : {0u,0x40000000u}) {
            gate.lifecycle(mirror);
            for (const auto count : {1u,3u,17u,75u}) for (const bool provider : {false,true}) gate.caller(count,mirror,provider);
        }
        std::ofstream out(argv[2]); require(bool(out),"Cannot create report");
        out << "{\"schema_version\":1,\"scope\":\"original_heap_lifecycle_and_caller_tail\",\"success\":true,"
            << "\"elf_sha256\":\"" << kElfHash << "\",\"original_calls_per_path\":" << gate.calls
            << ",\"allocation_calls\":" << gate.allocations << ",\"free_calls\":" << gate.frees
            << ",\"initializations_and_resets\":" << gate.resets << ",\"caller_tail_cases\":" << gate.caller_cases
            << ",\"same_address_reuses\":" << gate.reused_addresses
            << ",\"reverse_requested_alignment_residues\":" << gate.reverse_alignment_residues
            << ",\"max_interpreter_slices\":" << gate.max_slices
            << ",\"virtual_source_provider_cases\":" << gate.virtual_provider_cases
            << ",\"full_ram_vram_cpu_compared\":true,\"virtual_source_provider_executed\":true,"
            << "\"object_constructor_and_resource_loader_executed\":false,"
            << "\"live_allocation_authority_installed\":false}\n";
        out.close(); require(bool(out),"Report write failed");
        std::cout << "Original allocation gate: " << gate.calls << " calls, " << gate.caller_cases
                  << " caller tails, " << gate.reused_addresses << " address reuses; passed\n";
        return 0;
    } catch(const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
