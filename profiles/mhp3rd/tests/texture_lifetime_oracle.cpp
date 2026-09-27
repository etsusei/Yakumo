#include "native/bridge_contracts.hpp"
#include "native/texture_command_dispatch.hpp"
#include "native/texture_lifetime_tracker.hpp"
#include "overlay_module.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
using namespace mhp3rd::native;
constexpr std::uint32_t end_pc = 0x08001000u, container = 0x08200000u;
constexpr std::uint32_t owner_manager = container + 4u, command_manager = 0x08202000u;
constexpr std::uint32_t owner_heap = 0x09200000u, command_heap = 0x09000000u;
constexpr std::uint32_t sp = 0x08400800u, overlay_base = 0x0A05E600u;
constexpr std::uint32_t ctor = 0x0A0E7460u;
constexpr mhp3rd::resources::SourceCodeIdentity supported_code{
    0xF6300296C8D954E5ull, 1u, 0x088BD058u, 0x088B0114u, 0x088B7DE0u,
    0x088652C4u, 0x08865378u};
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
    require(at + 4u <= bytes.size(), "Header field outside image");
    return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1u]) << 8u) |
        (std::uint32_t(bytes[at + 2u]) << 16u) | (std::uint32_t(bytes[at + 3u]) << 24u);
}
struct Library {
    void *handle{};
    explicit Library(const char *path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        require(handle != nullptr, "Could not load original lobby module");
    }
    ~Library() { if (handle) dlclose(handle); }
};
struct Event {
    TextureLifetimeCheckpoint checkpoint{};
    AllegrexContext cpu{};
};
struct Observations {
    std::array<Event, 256> events{};
    std::size_t count{};
    bool overflow{};
    unsigned builders{};
    TextureLifetimeTracker *tracker{};
    std::optional<TextureBuilderTicket> last_ticket;
    std::optional<TextureLifetimeCheckpoint> drop;
    bool replay_accepted{};
    bool switch_at_builder{};
    static void lifetime(void *data, const Runtime &runtime, const AllegrexContext &ctx,
                         TextureLifetimeCheckpoint checkpoint) noexcept {
        auto &self = *static_cast<Observations *>(data);
        if (self.count == self.events.size()) { self.overflow = true; return; }
        self.events[self.count++] = {checkpoint, ctx};
        if (self.tracker && self.drop != checkpoint) self.tracker->observe(runtime, ctx, checkpoint);
        if (self.switch_at_builder && checkpoint == TextureLifetimeCheckpoint::BuilderCall) {
            const auto uid = runtime_thread_uid();
            set_runtime_thread_identity(uid == 3001 ? 3002 : 3001, "lifetime fault fixture");
            set_runtime_thread_identity(uid, "lifetime fixture restored");
        }
    }
    static bool entry(void *data, Runtime &runtime, AllegrexContext &ctx) noexcept {
        auto &self = *static_cast<Observations *>(data);
        ++self.builders;
        if (self.tracker) {
            self.last_ticket = self.tracker->consume_builder_ticket(runtime, ctx);
            if (self.last_ticket && self.tracker->consume_builder_ticket(runtime, ctx)) self.replay_accepted = true;
        }
        return false;
    }
    static void returned(void *, Runtime &, AllegrexContext &, std::uint32_t) noexcept {}
    const Event &one(TextureLifetimeCheckpoint point, std::size_t begin) const {
        const Event *found = nullptr;
        for (std::size_t i = begin; i < count; ++i) if (events[i].checkpoint == point) {
            require(found == nullptr, "Duplicate selected lifetime checkpoint");
            found = &events[i];
        }
        require(found != nullptr, "Missing selected lifetime checkpoint");
        return *found;
    }
};

class Gate {
    Runtime actual_, reference_;
    Observations observed_;
    TextureCommandDispatch binding_;
    std::uint32_t mirror_{};
    struct CodeRegion { std::uint32_t address{}; std::vector<std::uint8_t> bytes; };
    std::vector<CodeRegion> code_;
    std::unique_ptr<mhp3rd::resources::SourceAuthority> authority_;
    std::unique_ptr<TextureLifetimeTracker> tracker_;
    bool allow_loss_{};
    static bool code_valid(void *user, const Runtime &runtime, const AllegrexContext &,
                           const mhp3rd::resources::SourceCodeIdentity &identity) noexcept {
        const auto &self = *static_cast<const Gate *>(user);
        if (identity != supported_code || self.code_.empty()) return false;
        for (const auto &region : self.code_) {
            const auto *bytes = runtime.memory().raw_pointer(region.address, region.bytes.size());
            if (!bytes || std::memcmp(bytes, region.bytes.data(), region.bytes.size()) != 0) return false;
        }
        return true;
    }
public:
    unsigned calls{}, factories{}, caller_tails{}, owner_reuses{}, command_frees{}, owner_resets{}, heap_resets{}, max_steps{};
    Gate(const Elf32Image &elf, std::span<const std::uint8_t> overlay, Library &library,
         std::uint32_t mirror)
        : actual_(elf.required_ram_size()), reference_(elf.required_ram_size()),
          binding_(actual_, {&observed_, &Observations::entry, &Observations::returned, &Observations::lifetime}),
          mirror_(mirror) {
        require(binding_.installed(), "Could not bind lifetime observations");
        for (auto *runtime : {&actual_, &reference_}) {
            (void)elf.load_and_relocate(runtime->memory());
            runtime->memory().copy_in(overlay_base, overlay);
            runtime->memory().zero(overlay_base + static_cast<std::uint32_t>(overlay.size()), word(overlay, 20u));
        }
        register_generated_functions(actual_);
        auto info = reinterpret_cast<const mhp3rd::OverlayModuleInfo *(*)()>(dlsym(library.handle, "mhp3rd_overlay_info"));
        auto install = reinterpret_cast<void (*)(Runtime &)>(dlsym(library.handle, "mhp3rd_register_overlay"));
        require(info && install && info()->base == overlay_base &&
                info()->abi_version == mhp3rd::kOverlayAbiVersion &&
                info()->hash == 0xF6300296C8D954E5ull, "Original module identity differs");
        install(actual_);
        for (const auto &section : elf.sections()) {
            if ((section.flags & 4u) == 0u || section.size == 0u) continue;
            CodeRegion region{elf.section_runtime_address(section), std::vector<std::uint8_t>(section.size)};
            actual_.memory().copy_out(region.address, region.bytes);
            code_.push_back(std::move(region));
        }
        code_.push_back({overlay_base, std::vector<std::uint8_t>(overlay.begin(),
            overlay.begin() + 64u + word(overlay, 12u))});
        mhp3rd::resources::SourceAuthorityConfig config;
        config.ram_bytes = static_cast<std::uint32_t>(actual_.memory().size());
        config.owner_allocator = owner_manager; config.command_allocator = command_manager;
        config.supported_code = supported_code;
        authority_ = std::make_unique<mhp3rd::resources::SourceAuthority>(config);
        TextureLifetimeTrackerConfig tracker_config;
        tracker_config.owner_allocator = owner_manager; tracker_config.command_allocator = command_manager;
        tracker_config.code = supported_code;
        tracker_config.current_code = &code_valid; tracker_config.current_code_user = this;
        tracker_ = std::make_unique<TextureLifetimeTracker>(actual_, *authority_, tracker_config);
        observed_.tracker = tracker_.get();
        store(0x09FBE8D8u, raw(container));
        store(0x09FBE75Cu, raw(command_manager));
    }
    std::uint32_t raw(std::uint32_t value) const { return value | mirror_; }
    TextureLifetimeTrackerStats tracker_stats() const { return tracker_->stats(); }
    void store(std::uint32_t address, std::uint32_t value) {
        actual_.memory().store32(address, value); reference_.memory().store32(address, value);
    }
    AllegrexContext context(std::uint32_t entry) const {
        AllegrexContext ctx{};
        for (std::size_t i = 1; i < ctx.gpr.size(); ++i) ctx.gpr[i] = 0x34567000u + static_cast<std::uint32_t>(i);
        ctx.gpr[29] = raw(sp); ctx.gpr[31] = end_pc; ctx.pc = entry;
        return ctx;
    }
    AllegrexContext run(AllegrexContext ctx) {
        auto expected = ctx;
        unsigned dispatches = 0;
        do {
            require(++dispatches < 10000u && actual_.has_function(ctx.pc), "AOT lifetime path escaped dispatch budget");
            require(actual_.invoke_isolated_aot(ctx.pc, ctx) && !actual_.stopped(), "AOT lifetime path stopped");
        } while (ctx.pc != end_pc);
        unsigned steps = 0;
        do {
            require((expected.pc >= 0x08804000u && expected.pc < 0x08965A00u) ||
                    (expected.pc >= ctor && expected.pc < ctor + 112u), "Interpreter escaped certified main/constructor code");
            require(interpret_allegrex(reference_, expected, 1u) == InterpreterExit::Budget &&
                    !reference_.stopped(), "Interpreter lifetime path stopped");
            require(++steps < 2000000u, "Lifetime path exceeded instruction budget");
        } while (expected.pc != end_pc);
        require(same_context(ctx, expected), "Lifetime observer changed original CPU effects");
        require(actual_.memory().bytes() == reference_.memory().bytes(), "Lifetime observer changed original RAM effects");
        require(actual_.memory().vram_bytes() == reference_.memory().vram_bytes(), "Lifetime observer changed original VRAM effects");
        require(!observed_.overflow, "Lifetime observation capacity exceeded");
        require(!observed_.replay_accepted, "One-use builder ticket was replayed");
        require(allow_loss_ || tracker_->error() == TextureLifetimeTrackerError::None,
                "Actual lifetime tracker rejected a certified path: " +
                std::to_string(static_cast<int>(tracker_->error())));
        ++calls; max_steps = std::max(max_steps, steps);
        return ctx;
    }
    void initialize(std::uint32_t manager, std::uint32_t heap) {
        const auto first = observed_.count;
        auto ctx = context(0x08879DA4u);
        ctx.gpr[4] = raw(manager); ctx.gpr[5] = raw(heap); ctx.gpr[6] = 0x100000u;
        (void)run(ctx);
        require(observed_.one(TextureLifetimeCheckpoint::HeapInit, first).cpu.gpr[4] == raw(manager) &&
                observed_.one(TextureLifetimeCheckpoint::HeapReset, first).cpu.gpr[4] == raw(manager),
                "Composed unit did not observe init/reset entries");
    }
    std::uint32_t factory(bool owner_expected = true) {
        const auto first = observed_.count;
        store(raw(container + 0xB43130u), 0xFFFFFFFFu);
        for (std::uint32_t i = 0; i < 32u; i += 4u) store(raw(sp + i), 0x63120000u + i);
        store(raw(sp + 20u), end_pc);
        auto ctx = context(0x088BD058u);
        ctx.gpr[17] = 0x1234u;
        const auto owner = run(ctx).gpr[2];
        require(owner != 0u, "Original factory allocation failed");
        const auto &allocation = observed_.one(TextureLifetimeCheckpoint::ReverseAllocate, first).cpu;
        const auto &result = observed_.one(TextureLifetimeCheckpoint::FactoryAllocationResult, first).cpu;
        const auto &construct = observed_.one(TextureLifetimeCheckpoint::FactoryConstructorCall, first).cpu;
        const auto &finished = observed_.one(TextureLifetimeCheckpoint::FactoryConstructorResult, first).cpu;
        require(allocation.gpr[4] == raw(owner_manager) && allocation.gpr[5] == 0x2F470u &&
                allocation.gpr[6] == 16u && allocation.gpr[31] == 0x088BD07Cu &&
                result.gpr[2] == owner && construct.gpr[4] == owner && finished.gpr[2] == 1u,
                "Factory checkpoints broke allocation-to-constructor provenance");
        require(actual_.memory().load32(owner) == 0x0896FBC8u &&
                actual_.memory().load32(raw(container + 0xB43134u)) == owner,
                "Original factory did not record the constructed allocation");
        ++factories;
        if (owner_expected)
            require(tracker_->owner_token(owner).has_value(), "Factory did not issue a live owner lease");
        return owner;
    }
    std::uint32_t caller(std::uint32_t owner, bool ticket_expected = true, std::uint32_t count = 1u) {
        const auto first = observed_.count;
        const auto root = owner + 0x27C70u;
        // Deliberate fixture input, not an observed transfer or readiness receipt.
        store(root, 3u); store(root + 20u, 32u); store(root + 24u, 56u);
        store(root + 40u, count); store(root + 48u, 40u); store(root + 56u, 1u);
        store(root + 64u, 24u); store(root + 72u, 8u); store(root + 76u, 4u | (4u << 16u));
        for (std::uint32_t i = 0; i < 128u; i += 4u) store(raw(sp + i), 0x78230000u + i);
        store(raw(sp + 116u), end_pc);
        auto ctx = context(0x088B0398u); ctx.gpr[30] = owner;
        const auto before_builders = observed_.builders;
        (void)run(ctx);
        const auto &tail = observed_.one(TextureLifetimeCheckpoint::CallerTail, first).cpu;
        const auto &provider = observed_.one(TextureLifetimeCheckpoint::ProviderResult, first).cpu;
        const auto &child = observed_.one(TextureLifetimeCheckpoint::ChildResult, first).cpu;
        const auto &allocation = observed_.one(TextureLifetimeCheckpoint::ForwardAllocate, first).cpu;
        const auto &result = observed_.one(TextureLifetimeCheckpoint::CommandAllocationResult, first).cpu;
        const auto &builder = observed_.one(TextureLifetimeCheckpoint::BuilderCall, first).cpu;
        const auto command = actual_.memory().load32(owner + 0x13F4u);
        require(tail.gpr[30] == owner && provider.gpr[2] == root && child.gpr[2] == root + 32u &&
                allocation.gpr[4] == raw(command_manager) && allocation.gpr[5] == count * 36u &&
                allocation.gpr[6] == 16u && result.gpr[2] == command && builder.gpr[5] == command &&
                builder.gpr[6] == root + 32u && observed_.builders == before_builders + 1u,
                "Selected caller observations did not match actual original arguments/results");
        ++caller_tails;
        if (!ticket_expected) {
            require(!observed_.last_ticket, "Ineligible caller received a builder ticket");
            return command;
        }
        require(observed_.last_ticket.has_value() &&
                observed_.last_ticket->raw_owner == owner && observed_.last_ticket->raw_child == root + 32u &&
                observed_.last_ticket->raw_command == command && observed_.last_ticket->requested_command_bytes == 36u,
                "Actual caller checkpoints did not issue the expected one-use ticket");
        const auto &ticket = *observed_.last_ticket;
        require(authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                authority_->permit(ticket.owner, ticket.command, 32u, 56u, 0u, 36u, supported_code).error ==
                    mhp3rd::resources::AuthorityError::NotReady,
                "Allocation/caller events fabricated source readiness without a transfer");
        return command;
    }
    void check() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        auto owner = factory();
        const auto old_owner = *tracker_->owner_token(owner);
        auto command = caller(owner);
        const auto old_command = *tracker_->command_token(command);
        const auto revision = authority_->revision();
        store(owner + 0xB80u, 3u); // Exercise the original no-pending-cancel reset branch.
        auto reset = context(0x088A53C8u); reset.gpr[4] = owner;
        const auto reset_first = observed_.count;
        (void)run(reset);
        require(observed_.one(TextureLifetimeCheckpoint::OwnerReset, reset_first).cpu.gpr[4] == owner &&
                *tracker_->owner_token(owner) == old_owner && authority_->revision() > revision,
                "Owner reset lost its lease or missed source invalidation");
        ++owner_resets;
        auto first = observed_.count;
        auto release = context(0x088A3474u); release.gpr[4] = raw(command_manager); release.gpr[5] = owner;
        (void)run(release);
        require(observed_.one(TextureLifetimeCheckpoint::Free, first).cpu.gpr[5] == command &&
                actual_.memory().load32(owner + 0x13F4u) == 0u, "Original selected release was not observed");
        ++command_frees;
        require(!tracker_->command_token(command).has_value(), "Command free retained its live token");
        first = observed_.count;
        release = context(0x08879FF0u); release.gpr[4] = raw(owner_manager); release.gpr[5] = owner;
        (void)run(release);
        require(observed_.one(TextureLifetimeCheckpoint::Free, first).cpu.gpr[5] == owner,
                "Original owner free was not observed");
        require(!tracker_->owner_token(owner).has_value(), "Owner free retained its live token");
        require(factory() == owner, "Original owner address reuse not exercised");
        require(*tracker_->owner_token(owner) != old_owner, "Reused address retained an old allocation generation");
        ++owner_reuses;
        require(caller(owner) == command && *tracker_->command_token(command) != old_command,
                "Reused command address retained its previous generation");
        reset = context(0x08879D58u); reset.gpr[4] = raw(owner_manager);
        (void)run(reset);
        require(!tracker_->owner_token(owner) && tracker_->command_token(command).has_value(),
                "Heap reset missed its owners or invalidated the other heap");
        ++heap_resets;
    }
    void missing_constructor_checkpoint() {
        initialize(owner_manager, owner_heap);
        observed_.drop = TextureLifetimeCheckpoint::FactoryConstructorCall;
        allow_loss_ = true;
        const auto owner = factory(false);
        require(tracker_->error() == TextureLifetimeTrackerError::MissingPhase &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                !tracker_->owner_token(owner), "Missing constructor phase fabricated an owner lease");
    }
    void changed_code() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        const auto before = actual_.memory().load32(ctor + 108u);
        store(ctor + 108u, before ^ 1u); // This constructor is not executed by the subsequent caller.
        allow_loss_ = true;
        (void)caller(owner, false);
        require(tracker_->error() == TextureLifetimeTrackerError::CodeChanged &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost,
                "Changed current code did not revoke observations");
        store(ctor + 108u, before);
        require(!tracker_->owner_token(owner), "Restoring code silently restored stale authority");
    }
    void switched_thread() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        observed_.switch_at_builder = true; allow_loss_ = true;
        (void)caller(owner, false);
        require(tracker_->error() == TextureLifetimeTrackerError::ConflictingPhase &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost,
                "Switch-away/back accepted a stale caller ticket");
    }
    void empty_command() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        require(caller(owner, false, 0u) == 0u && tracker_->stats().commands_allocated == 0u &&
                tracker_->stats().active_frames == 0u && tracker_->stats().tickets_consumed == 0u,
                "No-command path leaked a positive lease or unfinished caller frame");
    }
};
}

int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: texture_lifetime_oracle EBOOT.ELF lobby.bin lobby.dylib report.json");
        require(!std::filesystem::exists(argv[4]), "Use a new report path");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c", "Unsupported ELF");
        require(sha256_file(argv[2]) == "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca", "Unsupported lobby image");
        require(sha256_file(argv[3]) == "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538", "Unsupported lobby module");
        std::ifstream stream(argv[2], std::ios::binary);
        std::vector<std::uint8_t> overlay{std::istreambuf_iterator<char>(stream), {}};
        const auto elf = Elf32Image::from_file(argv[1]);
        Library module(argv[3]);
        unsigned calls = 0, factories = 0, callers = 0, reuses = 0, frees = 0, resets = 0, heaps = 0, max_steps = 0;
        std::uint64_t owner_leases = 0, tickets = 0;
        for (auto mirror : {0u, 0x40000000u}) {
            Gate gate(elf, overlay, module, mirror); gate.check();
            calls += gate.calls; factories += gate.factories; callers += gate.caller_tails;
            reuses += gate.owner_reuses; frees += gate.command_frees;
            resets += gate.owner_resets; heaps += gate.heap_resets;
            max_steps = std::max(max_steps, gate.max_steps);
            owner_leases += gate.tracker_stats().owners_constructed;
            tickets += gate.tracker_stats().tickets_consumed;
        }
        for (unsigned fault = 0; fault < 4u; ++fault) {
            Gate gate(elf, overlay, module, 0u);
            if (fault == 0u) gate.missing_constructor_checkpoint();
            else if (fault == 1u) gate.changed_code();
            else if (fault == 2u) gate.switched_thread();
            else gate.empty_command();
            calls += gate.calls; max_steps = std::max(max_steps, gate.max_steps);
            factories += gate.factories; callers += gate.caller_tails;
            owner_leases += gate.tracker_stats().owners_constructed;
            tickets += gate.tracker_stats().tickets_consumed;
        }
        std::ofstream out(argv[4]);
        out << "{\"schema_version\":1,\"scope\":\"original-texture-lifetime-checkpoints\",\"success\":true,"
            << "\"calls_per_path\":" << calls << ",\"factory_chains\":" << factories
            << ",\"caller_chains\":" << callers << ",\"owner_reuses\":" << reuses
            << ",\"command_releases\":" << frees << ",\"max_interpreter_slices\":" << max_steps
            << ",\"owner_resets\":" << resets << ",\"heap_resets\":" << heaps
            << ",\"loss_cases\":3,\"owner_leases\":" << owner_leases << ",\"consumed_tickets\":" << tickets
            << ",\"no_command_cases\":1"
            << ",\"full_ram_vram_cpu_compared\":true,\"transfer_readiness\":false}\n";
        require(static_cast<bool>(out), "Could not write lifetime report");
        std::cout << "Lifetime: " << calls << " original calls, " << factories << " factory chains passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
