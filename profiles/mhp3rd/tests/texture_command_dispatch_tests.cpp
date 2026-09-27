#include "native/texture_command_dispatch.hpp"

#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using mhp3rd::native::TextureCommandCallbacks;
using mhp3rd::native::TextureCommandDispatch;
using mhp3rd::native::texture_command_entry;
using mhp3rd::native::texture_command_return;
using mhp3rd::native::texture_lifetime_checkpoint;
using mhp3rd::native::TextureLifetimeCheckpoint;
using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;

constexpr std::uint32_t kRamSize = 32u * 1024u * 1024u;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

AllegrexContext context_fixture(std::uint32_t seed) {
    AllegrexContext context{};
    const auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (auto &word : context.gpr) word = next();
    for (auto &value : context.fpr) value = std::bit_cast<float>(next());
    for (auto &value : context.vfpu) value = std::bit_cast<float>(next());
    for (auto &word : context.vfpu_ctrl) word = next();
    context.hi = next();
    context.lo = next();
    context.pc = next();
    context.fcr31 = next();
    context.fpr[0] = std::bit_cast<float>(0x7FC12345u);
    context.vfpu[1] = std::bit_cast<float>(0x80000000u);
    return context;
}

bool same_context(const AllegrexContext &left, const AllegrexContext &right) {
    return left.gpr == right.gpr && left.hi == right.hi &&
           left.lo == right.lo && left.pc == right.pc &&
           left.fcr31 == right.fcr31 && left.vfpu_ctrl == right.vfpu_ctrl &&
           std::memcmp(left.fpr.data(), right.fpr.data(), sizeof(left.fpr)) == 0 &&
           std::memcmp(left.vfpu.data(), right.vfpu.data(), sizeof(left.vfpu)) == 0;
}

struct Probe {
    std::size_t entries{};
    std::size_t returns{};
    Runtime *entry_runtime{};
    Runtime *return_runtime{};
    AllegrexContext *entry_context{};
    AllegrexContext *return_context{};
    AllegrexContext observed_entry{};
    AllegrexContext observed_return{};
    std::uint32_t return_pc{};
    bool handle{};
    std::uint32_t continuation{};
};

bool on_entry(void *user, Runtime &runtime, AllegrexContext &context) noexcept {
    auto &probe = *static_cast<Probe *>(user);
    ++probe.entries;
    probe.entry_runtime = &runtime;
    probe.entry_context = &context;
    probe.observed_entry = context;
    if (probe.handle) context.pc = probe.continuation;
    return probe.handle;
}

void on_return(void *user, Runtime &runtime, AllegrexContext &context,
               std::uint32_t return_pc) noexcept {
    auto &probe = *static_cast<Probe *>(user);
    ++probe.returns;
    probe.return_runtime = &runtime;
    probe.return_context = &context;
    probe.observed_return = context;
    probe.return_pc = return_pc;
}

TextureCommandCallbacks callbacks(Probe &probe) {
    return {&probe, on_entry, on_return};
}

void no_owner_preserves_full_state() {
    Runtime runtime(kRamSize);
    auto context = context_fixture(0xD3117A53u);
    const auto original = context;

    std::vector<std::uint8_t> ram(runtime.memory().size());
    std::vector<std::uint8_t> vram(runtime.memory().vram_size());
    std::uint32_t seed = 0x87654321u;
    const auto fill = [&seed](std::vector<std::uint8_t> &bytes) {
        for (auto &byte : bytes) {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<std::uint8_t>(seed >> 24u);
        }
    };
    fill(ram);
    fill(vram);
    runtime.memory().copy_in(GuestMemory::kPhysicalBase, ram);
    runtime.memory().copy_in(GuestMemory::kVramPhysicalBase, vram);

    require(!texture_command_entry(runtime, context), "Unowned entry was handled");
    texture_command_return(runtime, context, 0xFFFFFFFFu);
    require(same_context(context, original), "Unowned dispatch changed the CPU context");
    require(runtime.memory().bytes() == ram, "Unowned dispatch changed main RAM");
    require(runtime.memory().vram_bytes() == vram, "Unowned dispatch changed VRAM");
}

void callbacks_observe_complete_context_and_raw_return_pc() {
    Runtime runtime(kRamSize);
    Probe probe;
    TextureCommandDispatch binding(runtime, callbacks(probe));
    require(binding.installed(), "Valid callback pair did not install");

    auto context = context_fixture(0x1FA3C742u);
    const auto original = context;
    require(!texture_command_entry(runtime, context), "Declining callback was reported handled");
    require(probe.entries == 1u && probe.entry_runtime == &runtime &&
            probe.entry_context == &context, "Entry callback received the wrong target");
    require(same_context(probe.observed_entry, original),
            "Entry callback did not observe the complete original context");
    require(same_context(context, original), "Declined entry changed the CPU context");

    constexpr std::array<std::uint32_t, 4> return_pcs{
        0u, 0x08001003u, 0x88001000u, 0xFFFFFFFFu};
    for (std::size_t i = 0; i < return_pcs.size(); ++i) {
        texture_command_return(runtime, context, return_pcs[i]);
        require(probe.returns == i + 1u && probe.return_runtime == &runtime &&
                probe.return_context == &context, "Return callback received the wrong target");
        require(probe.return_pc == return_pcs[i], "Return PC was normalized or replaced");
        require(same_context(probe.observed_return, original),
                "Return callback did not observe the complete original context");
        require(same_context(context, original), "Observational return changed the CPU context");
    }

    probe.handle = true;
    probe.continuation = 0x88765433u;
    require(texture_command_entry(runtime, context), "Handled entry was reported declined");
    require(probe.entries == 2u && same_context(probe.observed_entry, original),
            "Handled callback did not receive the original context");
    auto expected = original;
    expected.pc = probe.continuation;
    require(same_context(context, expected), "Handled callback continuation was changed");
}

void runtime_isolation_and_duplicate_rejection() {
    Runtime first(kRamSize);
    Runtime second(kRamSize);
    Probe first_probe;
    Probe second_probe;
    Probe rejected_probe;
    TextureCommandDispatch first_binding(first, callbacks(first_probe));
    TextureCommandDispatch second_binding(second, callbacks(second_probe));
    require(first_binding.installed() && second_binding.installed(),
            "Independent runtimes did not both accept bindings");
    auto first_context = context_fixture(0x11223344u);
    auto second_context = context_fixture(0x55667788u);
    {
        TextureCommandDispatch duplicate(first, callbacks(rejected_probe));
        require(!duplicate.installed(), "A second owner installed on one runtime");
        require(!texture_command_entry(first, first_context), "First observer unexpectedly handled entry");
        texture_command_return(second, second_context, 0x12345679u);
    }
    require(first_probe.entries == 1u && first_probe.returns == 0u &&
            second_probe.entries == 0u && second_probe.returns == 1u &&
            rejected_probe.entries == 0u && rejected_probe.returns == 0u,
            "Callbacks crossed runtime or owner boundaries");
    require(first_probe.entry_runtime == &first && second_probe.return_runtime == &second,
            "Callback received another runtime");
    texture_command_return(first, first_context, 0xABCDEF01u);
    require(first_probe.returns == 1u && rejected_probe.returns == 0u,
            "Rejected duplicate or its destruction replaced the original callback");
}

void invalid_callbacks_do_not_install() {
    Runtime runtime(kRamSize);
    Probe probe;
    TextureCommandDispatch missing_both(runtime, {});
    TextureCommandDispatch missing_entry(runtime, {&probe, nullptr, on_return});
    TextureCommandDispatch missing_return(runtime, {&probe, on_entry, nullptr});
    require(!missing_both.installed() && !missing_entry.installed() &&
            !missing_return.installed(), "Incomplete callbacks installed");
    auto context = context_fixture(0x1234ABCDu);
    const auto original = context;
    require(!texture_command_entry(runtime, context), "Invalid callback handled entry");
    texture_command_return(runtime, context, 0xFFFFFFFFu);
    require(probe.entries == 0u && probe.returns == 0u && same_context(context, original),
            "Invalid callback changed state or received a call");
    TextureCommandDispatch valid(runtime, callbacks(probe));
    require(valid.installed(), "Rejected incomplete callbacks reserved the runtime");
    require(!texture_command_entry(runtime, context) && probe.entries == 1u,
            "Valid callback was unavailable after invalid registrations");
}

void destruction_detaches_and_allows_rebinding() {
    Runtime runtime(kRamSize);
    Probe first;
    Probe second;
    auto context = context_fixture(0x98ABCDEFu);
    const auto original = context;
    {
        TextureCommandDispatch binding(runtime, callbacks(first));
        require(binding.installed(), "Initial binding failed");
        require(!texture_command_entry(runtime, context), "Observer unexpectedly handled entry");
    }
    require(!texture_command_entry(runtime, context), "Destroyed owner still handled entry");
    texture_command_return(runtime, context, 0x13579BDFu);
    require(first.entries == 1u && first.returns == 0u && same_context(context, original),
            "Destroyed owner remained reachable");
    {
        TextureCommandDispatch binding(runtime, callbacks(second));
        require(binding.installed(), "Runtime could not be rebound after destruction");
        texture_command_return(runtime, context, 0x2468ACE1u);
        require(second.returns == 1u && second.return_pc == 0x2468ACE1u,
                "Rebound return callback did not run");
    }
}

void lifetime_only_observer_preserves_state() {
    Runtime runtime(kRamSize);
    struct Seen { unsigned calls{}; const Runtime *runtime{}; AllegrexContext cpu{};
                  TextureLifetimeCheckpoint checkpoint{}; } seen;
    TextureCommandCallbacks hooks;
    hooks.user = &seen;
    hooks.lifetime = [](void *user, const Runtime &target, const AllegrexContext &ctx,
                        TextureLifetimeCheckpoint point) noexcept {
        auto &value = *static_cast<Seen *>(user);
        ++value.calls; value.runtime = &target; value.cpu = ctx; value.checkpoint = point;
    };
    TextureCommandDispatch binding(runtime, hooks);
    require(binding.installed(), "A complete lifetime-only observer did not install");
    auto ctx = context_fixture(0x12349876u);
    const auto before = ctx;
    const auto ram = runtime.memory().bytes(), vram = runtime.memory().vram_bytes();
    require(!texture_command_entry(runtime, ctx), "Lifetime-only observer replaced a builder");
    texture_command_return(runtime, ctx, 0x08123456u);
    texture_lifetime_checkpoint(runtime, ctx, TextureLifetimeCheckpoint::FactoryAllocationResult);
    require(seen.calls == 1u && seen.runtime == &runtime && same_context(seen.cpu, before) &&
            seen.checkpoint == TextureLifetimeCheckpoint::FactoryAllocationResult,
            "Lifetime observer substituted PC or context");
    require(same_context(ctx, before) && runtime.memory().bytes() == ram && runtime.memory().vram_bytes() == vram,
            "Lifetime-only observer mutated guest state");
}

void capacity_exhaustion_and_refill() {
    constexpr std::size_t capacity = TextureCommandDispatch::kMaxRuntimes;
    std::array<std::unique_ptr<Runtime>, capacity + 1u> runtimes;
    std::array<std::unique_ptr<TextureCommandDispatch>, capacity + 1u> bindings;
    std::array<Probe, capacity + 1u> probes{};
    for (auto &runtime : runtimes) runtime = std::make_unique<Runtime>(kRamSize);
    for (std::size_t i = 0; i < capacity; ++i) {
        bindings[i] = std::make_unique<TextureCommandDispatch>(*runtimes[i], callbacks(probes[i]));
        require(bindings[i]->installed(), "Registry filled before its declared capacity");
    }
    bindings[capacity] = std::make_unique<TextureCommandDispatch>(
        *runtimes[capacity], callbacks(probes[capacity]));
    require(!bindings[capacity]->installed(), "Registry accepted an owner beyond capacity");

    auto context = context_fixture(0xCAFE7201u);
    require(!texture_command_entry(*runtimes[capacity], context) &&
            probes[capacity].entries == 0u, "Exhausted owner received a callback");
    require(!texture_command_entry(*runtimes[0], context) && probes[0].entries == 1u,
            "Exhaustion disturbed an installed owner");

    bindings[7].reset();
    bindings[capacity].reset();
    bindings[capacity] = std::make_unique<TextureCommandDispatch>(
        *runtimes[capacity], callbacks(probes[capacity]));
    require(bindings[capacity]->installed(), "Released capacity could not be refilled");
    require(!texture_command_entry(*runtimes[capacity], context) &&
            probes[capacity].entries == 1u, "Refilled slot did not dispatch");
    require(!texture_command_entry(*runtimes[7], context) && probes[7].entries == 0u,
            "Released runtime still dispatched");
    bindings[7] = std::make_unique<TextureCommandDispatch>(*runtimes[7], callbacks(probes[7]));
    require(!bindings[7]->installed(), "Registry exceeded capacity after refill");
}
} // namespace

int main() {
    struct TestCase { const char *name; void (*run)(); };
    constexpr std::array tests{
        TestCase{"no owner preserves full state", no_owner_preserves_full_state},
        TestCase{"callbacks preserve context and return PC", callbacks_observe_complete_context_and_raw_return_pc},
        TestCase{"runtime isolation and duplicate rejection", runtime_isolation_and_duplicate_rejection},
        TestCase{"invalid callbacks", invalid_callbacks_do_not_install},
        TestCase{"destruction and rebinding", destruction_detaches_and_allows_rebinding},
        TestCase{"lifetime-only observation", lifetime_only_observer_preserves_state},
        TestCase{"capacity exhaustion and refill", capacity_exhaustion_and_refill},
    };
    for (const auto &test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception &error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
