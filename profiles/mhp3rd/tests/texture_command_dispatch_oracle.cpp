#include "native/texture_command_dispatch.hpp"
#include "native/texture_commands_bridge.hpp"
#include "native/bridge_contracts.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace psprecomp;
using namespace mhp3rd::native;
constexpr std::uint32_t entry = 0x0889E5C0u;
constexpr std::uint32_t predecessor = entry - 4u;
constexpr std::uint32_t external_return = 0x08001000u;
constexpr std::uint32_t source = 0x09000000u;
constexpr std::uint32_t state = 0x08201020u;
constexpr std::uint32_t commands = 0x08220040u;
constexpr std::uint32_t stack = 0x08310080u;
constexpr TextureCommandBounds bounds{56u, 1u, 1u};
enum class Mode { Observe, Verify, Commit };
enum class Route { Registered, DirectChain, LocalFallthrough };
void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}

// This fixture supplies explicit offline bounds, not a fabricated production
// SourceAuthority. The production controller and event producers are separate.
struct Capture {
    Mode mode{Mode::Observe};
    bool stop_return{}, second_entry_exit{}, refuse{}, corrupt_prediction{};
    unsigned entries{}, returns{}, verified{}, committed{}, mismatch{};
    std::uint32_t entry_pc{}, return_pc{};
    bool failed{};
    TextureCommandBounds supplied_bounds{bounds};
    std::uint32_t caller_owner{}, caller_child{}, observed_command{};
    TextureCommandPlan plan;
    RuntimeExecutionContextToken token{};

    static bool on_entry(void *data, Runtime &runtime, AllegrexContext &ctx) noexcept {
        auto &self = *static_cast<Capture *>(data);
        ++self.entries;
        self.entry_pc = ctx.pc;
        if (self.caller_owner != 0u) {
            self.observed_command = ctx.gpr[5];
            if (ctx.gpr[30] != self.caller_owner || ctx.gpr[16] != self.caller_child ||
                ctx.gpr[4] != self.caller_owner + 0x13F0u || ctx.gpr[6] != self.caller_child ||
                ctx.gpr[31] != 0x088B0400u || ctx.gpr[7] != 0u || ctx.gpr[8] != 0u ||
                ctx.gpr[9] != 0u || ctx.gpr[5] == 0u ||
                runtime.memory().load32(self.caller_owner + 0x13F4u) != 0xDEADBEEFu) {
                self.failed = true;
                return false;
            }
        }
        // A controlled second entry proves Native's local redispatch. It is
        // a harness terminator, not a supported second builder invocation.
        if (self.second_entry_exit && self.entries == 2u) {
            ctx.pc = external_return;
            return true;
        }
        if (self.mode == Mode::Observe || self.refuse) return false;
        try {
            auto projected = ctx;
            projected.pc = entry;
            auto predicted = projected;
            if (self.corrupt_prediction) predicted.gpr[10] ^= 1u;
            self.plan = prepare_texture_commands(runtime.memory(), predicted, self.supplied_bounds);
            if (!self.plan.ok()) { self.failed = true; return false; }
            self.token = capture_runtime_execution_context();
            if (self.mode == Mode::Verify) return false;
            if (!runtime_execution_context_matches(self.token)) {
                self.failed = true;
                return false;
            }
            const auto result = commit_texture_commands(runtime.memory(), projected, self.plan);
            if (!result.ok()) { self.failed = true; return false; }
            ctx = projected;
            ++self.committed;
            return true;
        } catch (...) {
            self.failed = true;
            // A diagnostic exception during commit can have partial stores;
            // this harness must never execute original fallback afterward.
            if (self.mode == Mode::Commit) {
                runtime.stop("Texture dispatch fixture commit exception");
                return true;
            }
            return false;
        }
    }
    static void on_return(void *data, Runtime &runtime, AllegrexContext &ctx,
                          std::uint32_t target) noexcept {
        auto &self = *static_cast<Capture *>(data);
        ++self.returns;
        self.return_pc = target;
        if (self.mode == Mode::Verify && !self.refuse && !self.failed) {
            auto projected = ctx;
            projected.pc = target;
            if (!runtime_execution_context_matches(self.token)) self.failed = true;
            else if (compare_texture_commands(self.plan, runtime.memory(), projected).ok())
                ++self.verified;
            else ++self.mismatch;
        }
        if (self.stop_return) runtime.stop("Bounded original return fixture");
    }
    TextureCommandCallbacks callbacks() {
        return {this, &on_entry, &on_return};
    }
};

AllegrexContext fixture(Runtime &runtime, const Elf32Image &elf, bool empty,
                        bool alias, std::uint32_t return_to, bool compiled = true) {
    (void)elf.load_and_relocate(runtime.memory());
    if (compiled) register_generated_functions(runtime);
    auto &memory = runtime.memory();
    const auto paint = [&](std::uint32_t address, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            memory.store8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(i * 29u + 71u));
    };
    paint(state - 16u, 64u);
    paint(commands - 16u, 96u);
    paint(stack - 128u, 160u);
    paint(source, bounds.source_bytes);
    memory.store32(source + 8u, empty ? 0u : 1u);
    memory.store32(source + 16u, 40u);
    memory.store32(source + 24u, 1u);
    memory.store32(source + 32u, 24u);
    memory.store32(source + 40u, 8u);
    memory.store16(source + 44u, 4u);
    memory.store16(source + 46u, 4u);
    memory.store32(0x08AB3668u, 0x12345678u);
    AllegrexContext ctx{};
    for (std::size_t i = 1; i < ctx.gpr.size(); ++i) ctx.gpr[i] = 0x714F0123u + static_cast<std::uint32_t>(i);
    for (std::size_t i = 0; i < ctx.fpr.size(); ++i)
        ctx.fpr[i] = std::bit_cast<float>(0x7FC00100u + static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < ctx.vfpu.size(); ++i)
        ctx.vfpu[i] = std::bit_cast<float>(0x80001000u + static_cast<std::uint32_t>(i));
    ctx.gpr[4] = state; ctx.gpr[5] = commands; ctx.gpr[6] = source;
    ctx.gpr[7] = 0; ctx.gpr[8] = 0; ctx.gpr[9] = 0;
    ctx.gpr[29] = stack; ctx.gpr[31] = return_to; ctx.pc = entry;
    if (alias) for (const auto reg : {4u, 5u, 6u, 29u}) ctx.gpr[reg] |= 0x40000000u;
    return ctx;
}

void run_case(const Elf32Image &elf, Mode mode, Route route, bool empty,
              bool alias, bool local_return, bool refuse, bool corrupt,
              unsigned &cases, unsigned &max_slices) {
    Runtime original(elf.required_ram_size()), actual(elf.required_ram_size());
    const auto target = local_return ? predecessor : external_return;
    auto expected = fixture(original, elf, empty, alias, target, false);
    auto ctx = fixture(actual, elf, empty, alias, target);
    const auto start = route == Route::LocalFallthrough ? predecessor : entry;
    expected.pc = start; ctx.pc = start;
    unsigned slices = 0;
    do {
        require((expected.pc >= predecessor && expected.pc < entry + 528u) ||
                (expected.pc >= 0x08876A10u && expected.pc < 0x08876AFCu),
                "Interpreter left the certified fixture spans");
        require(interpret_allegrex(original, expected, 1u) == InterpreterExit::Budget &&
                !original.stopped(), "Interpreter failed");
        require(++slices < 10000u, "Interpreter fixture exceeded bound");
    } while (expected.pc != target);
    max_slices = std::max(max_slices, slices);

    Capture capture;
    capture.mode = mode; capture.refuse = refuse; capture.corrupt_prediction = corrupt;
    capture.stop_return = local_return && mode != Mode::Commit;
    capture.second_entry_exit = local_return && mode == Mode::Commit;
    TextureCommandDispatch binding(actual, capture.callbacks());
    require(binding.installed(), "Could not bind fixture observer");
    if (route == Route::DirectChain) {
        ctx.pc = 0x08812344u; // A successful direct chain need not update it.
        auto fast = actual.memory().aot_fast_view();
        require(actual.invoke_chained_direct<&recomp_unit_0038_entry, 38u,
                (entry - 0x0889C000u) / 4u + 1u, entry>(ctx, &fast),
                "Actual compiled direct chain did not execute");
    } else if (route == Route::LocalFallthrough) {
        // The preceding original nop uses a goto to enter the builder; there
        // is no host wrapper or per-PC registration at that transition.
        auto fast = actual.memory().aot_fast_view();
        recomp_unit_0038_entry(actual, ctx,
            static_cast<std::uint16_t>((predecessor - 0x0889C000u) / 4u + 1u), fast);
    } else {
        require(actual.invoke_isolated_aot(entry, ctx), "Registered original builder missing");
    }
    require(!capture.failed, "Fixture callback failed");
    require(capture.entries == (capture.second_entry_exit ? 2u : 1u), "Entry count differs");
    const bool native = mode == Mode::Commit && !refuse;
    require(capture.returns == (native ? 0u : 1u), "Original return missing or duplicated");
    require(capture.committed == (native ? 1u : 0u), "Fixture commit count differs");
    const bool verifying = mode == Mode::Verify && !refuse;
    require(capture.verified == (verifying && !corrupt ? 1u : 0u) &&
            capture.mismatch == (verifying && corrupt ? 1u : 0u), "Verify outcome differs");
    if (capture.stop_return) require(actual.stopped(), "Bounded return stop ignored");
    else require(!actual.stopped(), "AOT execution unexpectedly stopped");
    if (capture.second_entry_exit) expected.pc = external_return;
    require(same_context(ctx, expected), "AOT callback path differs from original CPU");
    require(actual.memory().bytes() == original.memory().bytes(), "AOT callback path changed original RAM effects");
    require(actual.memory().vram_bytes() == original.memory().vram_bytes(), "AOT callback path changed VRAM");
    if (route == Route::LocalFallthrough && !capture.second_entry_exit)
        require(capture.entry_pc == predecessor, "Fixture did not exercise stale local PC");
    if (!native) require(capture.return_pc == target, "Return callback replaced actual RA");
    ++cases;
}

// Execute the actual selected caller tail: virtual selector-7 provider,
// indexed child-2 accessor, original command allocator, builder and epilogue.
// Owner construction and completed resource transfer remain explicit fixture
// inputs here; this is not a live authority receipt.
void run_caller_case(const Elf32Image &elf, Mode mode, std::uint32_t count,
                     bool alias, unsigned &cases, unsigned &max_slices) {
    constexpr std::uint32_t owner = 0x08300000u, manager = 0x08200000u;
    constexpr std::uint32_t heap = 0x09000000u, heap_bytes = 0x10000u;
    constexpr std::uint32_t caller_stack = 0x08400080u, caller = 0x088B0398u;
    Runtime original(elf.required_ram_size()), actual(elf.required_ram_size());
    auto expected = fixture(original, elf, false, alias, external_return, false);
    auto ctx = fixture(actual, elf, false, alias, external_return);
    const auto raw = [alias](std::uint32_t value) { return value | (alias ? 0x40000000u : 0u); };
    const auto interpret = [&](AllegrexContext &cpu) {
        unsigned slices = 0;
        while (cpu.pc != external_return) {
            const auto pc = cpu.pc;
            require((pc >= 0x08879D58u && pc < 0x0887A104u) ||
                    (pc >= caller && pc < 0x088B0434u) ||
                    (pc >= 0x088B7DE0u && pc < 0x088B7E04u) ||
                    (pc >= 0x088661BCu && pc < 0x08866234u) ||
                    (pc >= entry && pc < entry + 528u) ||
                    (pc >= 0x08876A10u && pc < 0x08876AFCu),
                    "Caller interpreter left certified spans");
            require(interpret_allegrex(original, cpu, 1u) == InterpreterExit::Budget &&
                    !original.stopped(), "Caller interpreter failed");
            require(++slices < 100000u, "Caller exceeded instruction bound");
        }
        max_slices = std::max(max_slices, slices);
    };
    ctx.pc = expected.pc = 0x08879DA4u;
    ctx.gpr[4] = expected.gpr[4] = raw(manager);
    ctx.gpr[5] = expected.gpr[5] = raw(heap);
    ctx.gpr[6] = expected.gpr[6] = heap_bytes;
    require(actual.invoke_isolated_aot(ctx.pc, ctx) && !actual.stopped(), "Heap init did not execute");
    interpret(expected);
    require(same_context(ctx, expected) && actual.memory().bytes() == original.memory().bytes(),
            "Original heap initialization differs");
    const auto root = raw(owner + 0x27C70u);
    for (auto *runtime : {&original, &actual}) {
        auto &memory = runtime->memory();
        memory.store32(raw(owner), 0x0896FBC8u);
        memory.store32(raw(owner + 0x13F4u), 0xDEADBEEFu);
        memory.store32(0x09FBE75Cu, raw(manager));
        memory.store32(root, 3u);
        memory.store32(root + 20u, 32u);
        memory.store32(root + 24u, 16u + count * 40u);
        memory.store32(root + 40u, count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto record = root + 48u + i * 40u;
            memory.store32(record, 40u);
            memory.store32(record + 8u, 1u);
            memory.store32(record + 16u, 24u);
            memory.store32(record + 24u, 8u);
            memory.store32(record + 28u, 4u | (4u << 16u));
        }
        for (std::uint32_t i = 0; i < 128u; i += 4u)
            memory.store32(raw(caller_stack + i), 0x61520100u + i);
        memory.store32(raw(caller_stack + 116u), external_return);
    }
    ctx.gpr[30] = expected.gpr[30] = raw(owner);
    ctx.gpr[29] = expected.gpr[29] = raw(caller_stack);
    ctx.gpr[31] = expected.gpr[31] = external_return;
    ctx.pc = expected.pc = caller;
    Capture capture;
    capture.mode = mode;
    capture.supplied_bounds = {16u + count * 40u, count, 4096u};
    capture.caller_owner = raw(owner);
    capture.caller_child = root + 32u;
    TextureCommandDispatch binding(actual, capture.callbacks());
    require(binding.installed(), "Caller fixture binding failed");
    require(actual.invoke_isolated_aot(caller, ctx) && !actual.stopped() && ctx.pc == external_return,
            "Original caller chain did not return");
    interpret(expected);
    require(!capture.failed && capture.entries == 1u &&
            capture.committed == (mode == Mode::Commit ? 1u : 0u) &&
            capture.returns == (mode == Mode::Commit ? 0u : 1u) &&
            capture.verified == (mode == Mode::Verify ? 1u : 0u) && capture.mismatch == 0u,
            "Caller did not supply one matched builder invocation");
    const auto command = actual.memory().load32(raw(owner + 0x13F4u));
    require(command == capture.observed_command && GuestMemory::canonical(command) >= heap + 32u &&
            std::uint64_t(GuestMemory::canonical(command)) + count * 36u <= heap + heap_bytes,
            "Actual allocator result was not passed into builder state");
    require(actual.memory().load32(raw(owner + 0x13F0u)) == root + 32u,
            "Actual child accessor result was not passed into builder state");
    require(same_context(ctx, expected) && actual.memory().bytes() == original.memory().bytes() &&
            actual.memory().vram_bytes() == original.memory().vram_bytes(),
            "Observed caller chain changed original CPU/RAM/VRAM effects");
    ++cases;
}
}

int main(int argc, char **argv) {
    try {
        require(argc == 3, "usage: texture_command_dispatch_oracle EBOOT.ELF report.json");
        const auto elf_hash = sha256_file(argv[1]);
        require(elf_hash == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
                "ELF is not the certified original");
        require(!std::filesystem::exists(argv[2]), "Report already exists; use a fresh path");
        const auto elf = Elf32Image::from_file(argv[1]);
        unsigned cases = 0, max_slices = 0;
        for (auto route : {Route::Registered, Route::DirectChain, Route::LocalFallthrough})
            for (auto mode : {Mode::Observe, Mode::Verify, Mode::Commit})
                for (bool alias : {false, true})
                    run_case(elf, mode, route, false, alias, false, false, false, cases, max_slices);
        for (auto route : {Route::Registered, Route::LocalFallthrough}) {
            for (bool empty : {false, true}) {
                run_case(elf, Mode::Verify, route, empty, false, true, false, false, cases, max_slices);
                run_case(elf, Mode::Observe, route, empty, true, false, false, false, cases, max_slices);
            }
            run_case(elf, Mode::Commit, route, false, false, true, false, false, cases, max_slices);
            run_case(elf, Mode::Commit, route, false, false, false, true, false, cases, max_slices);
            // Empty branch preserves r10, allowing a wrong prediction to be
            // detected without corrupting the executed original state.
            run_case(elf, Mode::Verify, route, true, false, false, false, true, cases, max_slices);
        }
        unsigned caller_cases = 0;
        for (auto mode : {Mode::Observe, Mode::Verify, Mode::Commit})
            for (auto count : {1u, 3u, 17u})
                for (bool alias : {false, true})
                    run_caller_case(elf, mode, count, alias, caller_cases, max_slices);
        std::ofstream report(argv[2]);
        require(static_cast<bool>(report), "Could not open report");
        report << "{\"schema_version\":1,\"scope\":\"texture-command-instrumented-aot\","
               << "\"success\":true,\"cases\":" << cases << ",\"max_interpreter_slices\":" << max_slices
               << ",\"caller_chain_cases\":" << caller_cases
               << ",\"full_ram_vram_cpu_compared\":true,\"production_authority\":false,"
               << "\"elf_sha256\":\"" << elf_hash << "\"}\n";
        require(static_cast<bool>(report), "Could not write report");
        std::cout << "texture command dispatch: " << cases << " entry/return and " << caller_cases
                  << " original caller-chain cases passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
