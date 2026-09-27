#include "resources/source_authority.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <vector>

namespace mhp3rd::resources {
namespace {

struct Interval {
    std::uint32_t begin{}, end{};
};

[[nodiscard]] bool overlaps(Interval a, Interval b) noexcept {
    return a.begin < b.end && b.begin < a.end;
}

[[nodiscard]] bool contains(Interval outer, Interval inner) noexcept {
    return outer.begin <= inner.begin && inner.end <= outer.end;
}

[[nodiscard]] std::optional<Interval> physical_interval(
    std::uint32_t raw, std::uint32_t bytes, std::uint32_t ram_bytes) noexcept {
    constexpr std::uint64_t ram_base = 0x08000000u;
    if (bytes == 0u) return std::nullopt;
    const auto raw_end = std::uint64_t{raw} + bytes;
    const auto physical = std::uint64_t{raw & SourceAuthority::kPhysicalMask};
    const auto physical_end = physical + bytes;
    if (raw_end > std::numeric_limits<std::uint32_t>::max() ||
        physical < ram_base || physical_end > ram_base + ram_bytes)
        return std::nullopt;
    return Interval{static_cast<std::uint32_t>(physical),
                    static_cast<std::uint32_t>(physical_end)};
}

[[nodiscard]] bool add_raw(std::uint32_t base, std::uint32_t offset,
                           std::uint32_t &out) noexcept {
    const auto sum = std::uint64_t{base} + offset;
    if (sum > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(sum);
    return true;
}

[[nodiscard]] bool valid_code(const SourceCodeIdentity &code) noexcept {
    return code.module != 0u && code.epoch != 0u && code.factory != 0u &&
           code.caller != 0u && code.provider != 0u &&
           code.copy_worker != 0u && code.transform_worker != 0u;
}

[[nodiscard]] bool valid_copy_mode(SourceCopyMode mode) noexcept {
    return mode == SourceCopyMode::Inline || mode == SourceCopyMode::Worker;
}

[[nodiscard]] std::uint64_t acquire_instance() noexcept {
    static std::atomic<std::uint64_t> next{1u};
    auto value = next.load(std::memory_order_relaxed);
    while (value != 0u && value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1u,
                std::memory_order_relaxed, std::memory_order_relaxed)) return value;
    }
    return 0u;
}

[[nodiscard]] AuthorityEvent result(AuthorityError error,
                                    bool recorded = true) noexcept {
    return AuthorityEvent{error, recorded};
}

} // namespace

struct SourceAuthority::Impl {
    struct Source {
        bool active{}, complete{}, poisoned{};
        bool flags_bound{}, deobfuscate{}, hash_flag{};
        AuthorityToken token{};
        std::uint32_t resource_id{}, group{}, total{}, queued{}, completed{};
        SourceDigestEvidence digest{SourceDigestEvidence::NotChecked};
        SourceCodeIdentity code{};
    };
    struct Owner {
        bool live{}, binding_valid{};
        AuthorityToken token{};
        std::uint32_t base{};
        Interval allocation{}, slot{};
        SourceCodeIdentity code{};
        Source source{};
    };
    struct Command {
        bool live{};
        AuthorityToken token{};
        std::uint32_t base{}, bytes{};
        Interval allocation{};
    };
    struct Descriptor {
        bool used{}, current{};
        AuthorityToken token{};
        Interval span{};
    };
    enum class Phase { Descriptor, Read, Copy, Transform, Postprocess, Terminal };
    struct Request {
        bool pending{}, eligible{}, cancelled{}, has_alternate{};
        bool phase_snapshot{}, terminal_snapshot{};
        AuthorityToken token{}, owner{}, load{}, descriptor{};
        SourceFragment spec{};
        Interval destination{}, alternate{}, descriptor_span{};
        bool has_descriptor_span{};
        Phase phase{Phase::Descriptor};
        bool digest_computed{};
    };
    struct External {
        bool pending{};
        AuthorityToken token{};
        Interval destination{};
    };

    explicit Impl(const SourceAuthorityConfig &configuration)
        : config(configuration), code(configuration.supported_code),
          instance(acquire_instance()),
          owners(std::min(configuration.max_allocations, std::size_t{4096u})),
          commands(std::min(configuration.max_allocations, std::size_t{4096u})),
          descriptors(std::min(configuration.max_descriptors, std::size_t{4096u})),
          requests(std::min(configuration.max_requests, std::size_t{4096u})),
          externals(std::min(configuration.max_external_writers, std::size_t{4096u})) {
        constexpr std::size_t hard_max = 4096u;
        if (instance == 0u) failure = AuthorityError::CounterExhausted;
        else if ((config.ram_bytes != 32u * 1024u * 1024u &&
                  config.ram_bytes != 64u * 1024u * 1024u) ||
                 config.owner_allocator == 0u || config.command_allocator == 0u ||
                 !valid_code(code) || config.serial_ceiling == 0u ||
                 config.max_allocations == 0u || config.max_requests == 0u ||
                 config.max_descriptors == 0u || config.max_external_writers == 0u ||
                 config.max_allocations > hard_max || config.max_requests > hard_max ||
                 config.max_descriptors > hard_max ||
                 config.max_external_writers > hard_max)
            failure = AuthorityError::InvalidConfig;
    }

    SourceAuthorityConfig config;
    SourceCodeIdentity code;
    std::uint64_t instance{}, revision{};
    AuthorityError failure{AuthorityError::None};
    std::vector<Owner> owners;
    std::vector<Command> commands;
    std::vector<Descriptor> descriptors;
    std::vector<Request> requests;
    std::vector<External> externals;
    SourcePermit saved_permit{};
    bool has_permit{};

    [[nodiscard]] bool touch() noexcept {
        if (failure != AuthorityError::None) return false;
        if (revision >= config.serial_ceiling) {
            failure = AuthorityError::CounterExhausted;
            has_permit = false;
            return false;
        }
        ++revision;
        has_permit = false;
        return true;
    }

    [[nodiscard]] AuthorityEvent closed() const noexcept {
        return result(failure, false);
    }

    [[nodiscard]] AuthorityEvent latch(AuthorityError error) noexcept {
        if (failure != AuthorityError::None) return closed();
        failure = error;
        has_permit = false;
        return result(error);
    }

    [[nodiscard]] AuthorityToken new_token() const noexcept {
        return AuthorityToken{instance, revision};
    }

    [[nodiscard]] Owner *owner(AuthorityToken token) noexcept {
        for (auto &record : owners)
            if (record.live && record.token == token) return &record;
        return nullptr;
    }
    [[nodiscard]] const Owner *owner(AuthorityToken token) const noexcept {
        for (const auto &record : owners)
            if (record.live && record.token == token) return &record;
        return nullptr;
    }
    [[nodiscard]] Command *command(AuthorityToken token) noexcept {
        for (auto &record : commands)
            if (record.live && record.token == token) return &record;
        return nullptr;
    }
    [[nodiscard]] const Command *command(AuthorityToken token) const noexcept {
        for (const auto &record : commands)
            if (record.live && record.token == token) return &record;
        return nullptr;
    }
    [[nodiscard]] Descriptor *descriptor(AuthorityToken token) noexcept {
        for (auto &record : descriptors)
            if (record.used && record.current && record.token == token)
                return &record;
        return nullptr;
    }
    [[nodiscard]] Request *request(AuthorityToken token) noexcept {
        for (auto &record : requests)
            if (record.pending && record.token == token) return &record;
        return nullptr;
    }
    [[nodiscard]] External *external(AuthorityToken token) noexcept {
        for (auto &record : externals)
            if (record.pending && record.token == token) return &record;
        return nullptr;
    }

    [[nodiscard]] bool current(const Request &request) const noexcept {
        const auto *record = owner(request.owner);
        if (record == nullptr || !record->binding_valid ||
            !record->source.active ||
            record->source.token != request.load ||
            record->source.code != code || code != config.supported_code)
            return false;
        for (const auto &descriptor_record : descriptors)
            if (descriptor_record.used && descriptor_record.current &&
                descriptor_record.token == request.descriptor) return true;
        return false;
    }

    void poison(Request &request) noexcept {
        request.eligible = false;
        auto *record = owner(request.owner);
        if (record != nullptr && record->source.active &&
            record->source.token == request.load) record->source.poisoned = true;
    }

    // `expected` excludes only the exact current request's own source from
    // poisoning. Other owners, requests and descriptor spans remain foreign.
    void foreign_write(Interval write, const Request *expected = nullptr) noexcept {
        for (auto &record : owners) {
            if (!record.live) continue;
            const Interval vptr{record.allocation.begin,
                                record.allocation.begin + 4u};
            if (overlaps(vptr, write)) {
                record.binding_valid = false;
                record.source.poisoned = true;
            }
            if (record.source.active && overlaps(record.slot, write) &&
                !(expected != nullptr && record.token == expected->owner &&
                  record.source.token == expected->load))
                record.source.poisoned = true;
        }
        for (auto &request_record : requests) {
            if (!request_record.pending || &request_record == expected) continue;
            if (overlaps(request_record.destination, write) ||
                (request_record.has_alternate &&
                 overlaps(request_record.alternate, write)) ||
                (request_record.has_descriptor_span &&
                 overlaps(request_record.descriptor_span, write)))
                poison(request_record);
        }
    }

    [[nodiscard]] bool allocation_overlap(Interval span) const noexcept {
        for (const auto &record : owners)
            if (record.live && overlaps(record.allocation, span)) return true;
        for (const auto &record : commands)
            if (record.live && overlaps(record.allocation, span)) return true;
        return false;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        std::size_t count = 0u;
        for (const auto &record : owners) if (record.live) ++count;
        for (const auto &record : commands) if (record.live) ++count;
        return count;
    }

    [[nodiscard]] bool pending_overlaps(Interval span) const noexcept {
        for (const auto &request_record : requests)
            if (request_record.pending &&
                (overlaps(request_record.destination, span) ||
                 (request_record.has_alternate &&
                  overlaps(request_record.alternate, span)))) return true;
        for (const auto &external_record : externals)
            if (external_record.pending &&
                overlaps(external_record.destination, span)) return true;
        return false;
    }

    // A changed observed write destination remains a hazard after the write.
    // Two disjoint destination regions exhaust this request's bounded record.
    [[nodiscard]] bool remember_destination(Request &request, Interval span) noexcept {
        if (contains(request.destination, span)) return true;
        if (!request.has_alternate) {
            request.alternate = span;
            request.has_alternate = true;
            return true;
        }
        if (contains(request.alternate, span)) return true;
        if (request.alternate.begin <= span.end &&
            span.begin <= request.alternate.end) {
            request.alternate.begin = std::min(request.alternate.begin, span.begin);
            request.alternate.end = std::max(request.alternate.end, span.end);
            return true;
        }
        return false;
    }
};

SourceAuthority::SourceAuthority(const SourceAuthorityConfig &config)
    : impl_(std::make_unique<Impl>(config)) {}
SourceAuthority::~SourceAuthority() = default;

AuthorityError SourceAuthority::failure() const noexcept { return impl_->failure; }
std::uint64_t SourceAuthority::revision() const noexcept { return impl_->revision; }

AuthorityIssue SourceAuthority::construct_owner(std::uint32_t base,
    std::uint32_t requested_bytes, std::uint32_t allocator,
    std::uint32_t constructed_vtable, const SourceCodeIdentity &code) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    auto allocation = physical_interval(base, requested_bytes, state.config.ram_bytes);
    if (allocation && state.allocation_overlap(*allocation))
        return {state.latch(AuthorityError::ObservationLost), {}};
    if (allocator != state.config.owner_allocator ||
        requested_bytes != kOwnerBytes || constructed_vtable != kLobbyVtable ||
        code != state.code || code != state.config.supported_code)
        return {result(AuthorityError::InvalidIdentity, false), {}};
    std::uint32_t slot_base{};
    if (!allocation || !add_raw(base, kSlotOffset, slot_base))
        return {result(AuthorityError::InvalidRange, false), {}};
    auto slot = physical_interval(slot_base, kSlotBytes, state.config.ram_bytes);
    if (!slot || !contains(*allocation, *slot))
        return {result(AuthorityError::InvalidRange, false), {}};
    if (state.live_allocations() >= state.config.max_allocations)
        return {state.latch(AuthorityError::NoCapacity), {}};
    auto it = std::find_if(state.owners.begin(), state.owners.end(),
                           [](const Impl::Owner &entry) { return !entry.live; });
    if (it == state.owners.end()) return {state.latch(AuthorityError::NoCapacity), {}};
    if (!state.touch()) return {state.closed(), {}};
    *it = Impl::Owner{};
    it->live = true;
    it->binding_valid = true;
    it->token = state.new_token();
    it->base = base;
    it->allocation = *allocation;
    it->slot = *slot;
    it->code = code;
    return {result(AuthorityError::None), it->token};
}

AuthorityEvent SourceAuthority::reset_owner(AuthorityToken owner) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.owner(owner);
    if (record == nullptr) return result(AuthorityError::StaleToken, false);
    if (!state.touch()) return state.closed();
    record->source = Impl::Source{};
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::release_owner(AuthorityToken owner) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.owner(owner);
    if (record == nullptr) return result(AuthorityError::StaleToken, false);
    if (!state.touch()) return state.closed();
    record->live = false;
    record->source = Impl::Source{};
    return result(AuthorityError::None);
}

AuthorityIssue SourceAuthority::allocate_command(std::uint32_t base,
    std::uint32_t requested_bytes, std::uint32_t allocator) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    auto allocation = physical_interval(base, requested_bytes, state.config.ram_bytes);
    if (allocation && state.allocation_overlap(*allocation))
        return {state.latch(AuthorityError::ObservationLost), {}};
    if (allocator != state.config.command_allocator)
        return {result(AuthorityError::InvalidIdentity, false), {}};
    if (!allocation) return {result(AuthorityError::InvalidRange, false), {}};
    if (state.live_allocations() >= state.config.max_allocations)
        return {state.latch(AuthorityError::NoCapacity), {}};
    auto it = std::find_if(state.commands.begin(), state.commands.end(),
                           [](const Impl::Command &entry) { return !entry.live; });
    if (it == state.commands.end()) return {state.latch(AuthorityError::NoCapacity), {}};
    if (!state.touch()) return {state.closed(), {}};
    *it = Impl::Command{};
    it->live = true;
    it->token = state.new_token();
    it->base = base;
    it->bytes = requested_bytes;
    it->allocation = *allocation;
    return {result(AuthorityError::None), it->token};
}

AuthorityEvent SourceAuthority::release_command(AuthorityToken command) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.command(command);
    if (record == nullptr) return result(AuthorityError::StaleToken, false);
    if (!state.touch()) return state.closed();
    record->live = false;
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::code_changed(const SourceCodeIdentity &code) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    if (!valid_code(code) || code.epoch <= state.code.epoch)
        return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    state.code = code;
    for (auto &record : state.owners) if (record.live) record.source = Impl::Source{};
    return result(AuthorityError::None);
}

AuthorityIssue SourceAuthority::begin_descriptor(std::uint32_t raw_address,
                                                  std::uint32_t descriptor_bytes) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    if (descriptor_bytes != 32u)
        return {state.latch(AuthorityError::ObservationLost), {}};
    auto span = physical_interval(raw_address, descriptor_bytes,
                                  state.config.ram_bytes);
    if (!span) return {state.latch(AuthorityError::ObservationLost), {}};
    auto it = std::find_if(state.descriptors.begin(), state.descriptors.end(),
        [span](const Impl::Descriptor &entry) {
            return entry.used && entry.span.begin == span->begin;
        });
    if (it == state.descriptors.end()) {
        it = std::find_if(state.descriptors.begin(), state.descriptors.end(),
            [](const Impl::Descriptor &entry) { return !entry.used; });
        if (it == state.descriptors.end())
            return {state.latch(AuthorityError::NoCapacity), {}};
    }
    // Overlapping ring storage, including a partially shifted descriptor,
    // retires every older generation sharing those bytes. Keep each distinct
    // start address in history so its old token can never become current again.
    for (auto &old : state.descriptors) {
        if (!old.used || !overlaps(old.span, *span)) continue;
        for (auto &request : state.requests)
            if (request.pending && request.descriptor == old.token)
                state.poison(request);
        old.current = false;
    }
    if (!state.touch()) return {state.closed(), {}};
    state.foreign_write(*span);
    it->used = true;
    it->current = true;
    it->span = *span;
    it->token = state.new_token();
    return {result(AuthorityError::None), it->token};
}

AuthorityIssue SourceAuthority::begin_load(const SourceLoad &load) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    auto *owner = state.owner(load.owner);
    if (owner == nullptr)
        return {state.latch(AuthorityError::ObservationLost), {}};
    // Any observed start/reload revokes the previous bytes, even when the new
    // route or identity is outside this module's supported contract.
    if (!state.touch()) return {state.closed(), {}};
    owner->source = Impl::Source{};
    if (!owner->binding_valid || load.route != SourceRoute::DataBinState8 ||
        load.code != state.code || load.code != owner->code ||
        load.code != state.config.supported_code)
        return {state.latch(AuthorityError::ObservationLost), {}};
    if (load.total_bytes == 0u || load.total_bytes > kSlotBytes ||
        load.resource_id > 0xFFFFu || load.group > 0xFFu)
        return {state.latch(AuthorityError::ObservationLost), {}};
    owner->source.active = true;
    owner->source.token = state.new_token();
    owner->source.resource_id = load.resource_id;
    owner->source.group = load.group;
    owner->source.total = load.total_bytes;
    owner->source.code = load.code;
    return {result(AuthorityError::None), owner->source.token};
}

AuthorityIssue SourceAuthority::begin_fragment(const SourceFragment &fragment) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    if (fragment.bytes == 0u || fragment.bytes >
        std::numeric_limits<std::uint32_t>::max() - 3u)
        return {state.latch(AuthorityError::ObservationLost), {}};
    std::uint32_t write_base{};
    if (!add_raw(fragment.destination, fragment.offset, write_base))
        return {state.latch(AuthorityError::ObservationLost), {}};
    const auto footprint = fragment.deobfuscate
        ? (fragment.bytes + 3u) & ~std::uint32_t{3u} : fragment.bytes;
    auto destination = physical_interval(write_base, footprint,
                                         state.config.ram_bytes);
    if (!destination) return {state.latch(AuthorityError::ObservationLost), {}};
    auto slot = std::find_if(state.requests.begin(), state.requests.end(),
                            [](const Impl::Request &r) { return !r.pending; });
    if (slot == state.requests.end())
        return {state.latch(AuthorityError::NoCapacity), {}};

    Impl::Owner *owner = nullptr;
    for (auto &record : state.owners)
        if (record.live && record.source.active &&
            record.source.token == fragment.load) { owner = &record; break; }
    const auto *descriptor = state.descriptor(fragment.descriptor);
    AuthorityError eligibility = AuthorityError::None;
    if (owner == nullptr || descriptor == nullptr)
        eligibility = AuthorityError::StaleToken;
    else {
        std::uint32_t slot_raw{};
        const auto logical_end = std::uint64_t{fragment.offset} + fragment.bytes;
        const bool slot_address = add_raw(owner->base, kSlotOffset, slot_raw) &&
                                  ((fragment.destination & kPhysicalMask) ==
                                   (slot_raw & kPhysicalMask));
        if (!slot_address || !contains(owner->slot, *destination) ||
            fragment.total_bytes != owner->source.total ||
            fragment.resource_id != owner->source.resource_id ||
            fragment.group != owner->source.group ||
            fragment.resource_id > 0xFFFFu || fragment.group > 0xFFu ||
            !valid_copy_mode(fragment.copy_mode) ||
            fragment.code != owner->source.code ||
            fragment.code != state.code ||
            fragment.first != (owner->source.queued == 0u) ||
            fragment.offset != owner->source.queued ||
            fragment.total_bytes == 0u || logical_end > fragment.total_bytes ||
            fragment.last != (logical_end == fragment.total_bytes) ||
            (fragment.hash_flag && !fragment.deobfuscate) ||
            (owner->source.flags_bound &&
             (fragment.deobfuscate != owner->source.deobfuscate ||
              fragment.hash_flag != owner->source.hash_flag)))
            eligibility = AuthorityError::InvalidReceipt;
        else if (owner->source.poisoned)
            eligibility = AuthorityError::RecordedIneligible;
    }
    if (!state.touch()) return {state.closed(), {}};
    *slot = Impl::Request{};
    slot->pending = true;
    slot->eligible = eligibility == AuthorityError::None;
    slot->token = state.new_token();
    slot->owner = owner != nullptr ? owner->token : AuthorityToken{};
    slot->load = fragment.load;
    slot->descriptor = fragment.descriptor;
    slot->spec = fragment;
    slot->destination = *destination;
    if (descriptor != nullptr) {
        slot->descriptor_span = descriptor->span;
        slot->has_descriptor_span = true;
    }
    if (eligibility != AuthorityError::None && owner != nullptr)
        state.poison(*slot);
    if (eligibility == AuthorityError::None && owner != nullptr) {
        owner->source.queued = fragment.offset + fragment.bytes;
        if (!owner->source.flags_bound) {
            owner->source.flags_bound = true;
            owner->source.deobfuscate = fragment.deobfuscate;
            owner->source.hash_flag = fragment.hash_flag;
        }
    }
    return {result(eligibility), slot->token};
}

AuthorityEvent SourceAuthority::observe_descriptor(AuthorityToken request,
    const SourceFragment &fresh) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    const auto phase = record->phase;
    if (phase == Impl::Phase::Descriptor) record->phase = Impl::Phase::Read;
    record->phase_snapshot = true;
    if (phase == Impl::Phase::Terminal) record->terminal_snapshot = true;
    if (!(fresh == record->spec)) {
        state.poison(*record);
        if (fresh.bytes == 0u || fresh.bytes >
            std::numeric_limits<std::uint32_t>::max() - 3u)
            return state.latch(AuthorityError::ObservationLost);
        std::uint32_t write_base{};
        if (!add_raw(fresh.destination, fresh.offset, write_base))
            return state.latch(AuthorityError::ObservationLost);
        const auto footprint = fresh.deobfuscate
            ? (fresh.bytes + 3u) & ~std::uint32_t{3u} : fresh.bytes;
        const auto alternate = physical_interval(write_base, footprint,
                                                 state.config.ram_bytes);
        if (!alternate) return state.latch(AuthorityError::ObservationLost);
        if (!state.remember_destination(*record, *alternate))
            return state.latch(AuthorityError::ObservationLost);
        return result(AuthorityError::InvalidIdentity);
    }
    if (!state.current(*record) || !record->eligible) {
        state.poison(*record);
        return result(AuthorityError::RecordedIneligible);
    }
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_read(AuthorityToken request,
                                              std::int64_t returned_bytes) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    if (record->phase != Impl::Phase::Read || !record->phase_snapshot) {
        state.poison(*record);
        return result(AuthorityError::InvalidOrder);
    }
    if (state.current(*record) && record->eligible &&
        state.owner(record->owner)->source.completed != record->spec.offset) {
        state.poison(*record);
        return result(AuthorityError::InvalidOrder);
    }
    record->phase_snapshot = false;
    if (returned_bytes < static_cast<std::int64_t>(record->spec.bytes)) {
        // A short, zero, or failing read can be retried. It contributes no
        // completed extent and gives no copy/transform permission.
        return result(AuthorityError::Retry);
    }
    if (returned_bytes != static_cast<std::int64_t>(record->spec.bytes)) {
        state.poison(*record);
        return result(AuthorityError::InvalidReceipt);
    }
    record->phase = Impl::Phase::Copy;
    if (!state.current(*record) || !record->eligible) {
        state.poison(*record);
        return result(AuthorityError::RecordedIneligible);
    }
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_copy(AuthorityToken request,
    std::uint32_t raw_destination, std::uint32_t bytes,
    SourceCopyMode observed_mode) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    const auto actual = physical_interval(raw_destination, bytes,
                                          state.config.ram_bytes);
    if (!actual) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) {
        state.foreign_write(*actual);
        return state.latch(AuthorityError::ObservationLost);
    }
    // A changed copy destination may subsequently receive a rounded transform
    // write. Hold that full potential footprint until terminal/quiescence.
    const auto possible_bytes = record->spec.deobfuscate &&
        bytes <= std::numeric_limits<std::uint32_t>::max() - 3u
        ? (bytes + 3u) & ~std::uint32_t{3u} : bytes;
    const auto possible = physical_interval(raw_destination, possible_bytes,
                                            state.config.ram_bytes);
    if (!possible || !state.remember_destination(*record, *possible)) {
        state.foreign_write(*actual);
        return state.latch(AuthorityError::ObservationLost);
    }
    const bool correct_phase = record->phase == Impl::Phase::Copy &&
                               record->phase_snapshot;
    const bool matched = correct_phase &&
                         actual->begin == record->destination.begin &&
                         actual->end == record->destination.begin +
                             record->spec.bytes &&
                         bytes == record->spec.bytes &&
                         valid_copy_mode(observed_mode) &&
                         observed_mode == record->spec.copy_mode;
    const bool current = state.current(*record);
    if (matched && current && record->eligible) state.foreign_write(*actual, record);
    else state.foreign_write(*actual);
    if (correct_phase) {
        record->phase = record->spec.deobfuscate
            ? Impl::Phase::Transform : Impl::Phase::Postprocess;
        record->phase_snapshot = false;
    }
    if (!matched) {
        state.poison(*record);
        return result(AuthorityError::InvalidReceipt);
    }
    if (!current || !record->eligible) {
        state.poison(*record);
        return result(AuthorityError::RecordedIneligible);
    }
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_transform(AuthorityToken request,
    std::uint32_t raw_destination, std::uint32_t footprint_bytes,
    bool digest_computed) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    const auto actual = physical_interval(raw_destination, footprint_bytes,
                                          state.config.ram_bytes);
    if (!actual) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) {
        state.foreign_write(*actual);
        return state.latch(AuthorityError::ObservationLost);
    }
    if (!state.remember_destination(*record, *actual)) {
        state.foreign_write(*actual);
        return state.latch(AuthorityError::ObservationLost);
    }
    const bool correct_phase = record->phase == Impl::Phase::Transform &&
                               record->phase_snapshot &&
                               record->spec.deobfuscate;
    const auto rounded = (record->spec.bytes + 3u) & ~std::uint32_t{3u};
    const bool matched = correct_phase &&
                         actual->begin == record->destination.begin &&
                         actual->end == record->destination.end &&
                         footprint_bytes == rounded &&
                         digest_computed == record->spec.hash_flag;
    const bool current = state.current(*record);
    if (matched && current && record->eligible) state.foreign_write(*actual, record);
    else state.foreign_write(*actual);
    if (correct_phase) {
        record->phase = Impl::Phase::Postprocess;
        record->phase_snapshot = false;
    }
    if (!matched) {
        state.poison(*record);
        return result(AuthorityError::InvalidReceipt);
    }
    record->digest_computed = digest_computed;
    if (!current || !record->eligible) {
        state.poison(*record);
        return result(AuthorityError::RecordedIneligible);
    }
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_postprocess(AuthorityToken request) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    if (record->phase != Impl::Phase::Postprocess || !record->phase_snapshot) {
        state.poison(*record);
        return result(AuthorityError::InvalidOrder);
    }
    record->phase = Impl::Phase::Terminal;
    record->phase_snapshot = false;
    if (!state.current(*record) || !record->eligible) {
        state.poison(*record);
        return result(AuthorityError::RecordedIneligible);
    }
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_terminal(AuthorityToken request,
                                                   bool successful) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    const bool complete = successful && record->phase == Impl::Phase::Terminal &&
                          record->terminal_snapshot &&
                          !record->cancelled && record->eligible &&
                          state.current(*record) &&
                          state.owner(record->owner)->source.completed ==
                              record->spec.offset;
    if (complete) {
        auto *owner = state.owner(record->owner);
        if (owner != nullptr) {
            const auto end = std::uint64_t{record->spec.offset} +
                             record->spec.bytes;
            if (end <= owner->source.total) {
                owner->source.completed = static_cast<std::uint32_t>(end);
                if (record->digest_computed)
                    owner->source.digest = SourceDigestEvidence::ComputedOnly;
                if (record->spec.last && end == owner->source.total)
                    owner->source.complete = true;
            } else {
                owner->source.poisoned = true;
            }
        }
    } else {
        state.poison(*record);
    }
    if (complete) {
        record->pending = false;
        return result(AuthorityError::None);
    }
    return result(AuthorityError::RecordedIneligible);
}

AuthorityEvent SourceAuthority::cancel_request(AuthorityToken request) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return result(AuthorityError::StaleToken, false);
    if (!state.touch()) return state.closed();
    record->cancelled = true;
    state.poison(*record);
    return result(AuthorityError::RecordedIneligible);
}

AuthorityEvent SourceAuthority::prove_request_quiescent(AuthorityToken request) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.request(request);
    if (record == nullptr) return result(AuthorityError::StaleToken, false);
    if (!state.touch()) return state.closed();
    state.poison(*record);
    record->pending = false;
    return result(AuthorityError::RecordedIneligible);
}

AuthorityIssue SourceAuthority::begin_external_write(
    std::uint32_t raw_destination, std::uint32_t bytes) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.closed(), {}};
    const auto destination = physical_interval(raw_destination, bytes,
                                               state.config.ram_bytes);
    if (!destination) return {state.latch(AuthorityError::ObservationLost), {}};
    auto it = std::find_if(state.externals.begin(), state.externals.end(),
                           [](const Impl::External &entry) {
                               return !entry.pending;
                           });
    if (it == state.externals.end())
        return {state.latch(AuthorityError::NoCapacity), {}};
    if (!state.touch()) return {state.closed(), {}};
    *it = Impl::External{};
    it->pending = true;
    it->token = state.new_token();
    it->destination = *destination;
    return {result(AuthorityError::None), it->token};
}

AuthorityEvent SourceAuthority::observe_external_write(AuthorityToken writer,
    std::uint32_t raw_destination, std::uint32_t bytes) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    const auto actual = physical_interval(raw_destination, bytes,
                                          state.config.ram_bytes);
    if (!actual) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    state.foreign_write(*actual);
    auto *record = state.external(writer);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!contains(record->destination, *actual))
        return state.latch(AuthorityError::ObservationLost);
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::prove_external_quiescent(AuthorityToken writer) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    auto *record = state.external(writer);
    if (record == nullptr) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    record->pending = false;
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observe_unknown_write(
    std::uint32_t raw_destination, std::uint32_t bytes) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.closed();
    const auto actual = physical_interval(raw_destination, bytes,
                                          state.config.ram_bytes);
    if (!actual) return state.latch(AuthorityError::ObservationLost);
    if (!state.touch()) return state.closed();
    state.foreign_write(*actual);
    return result(AuthorityError::None);
}

AuthorityEvent SourceAuthority::observer_lost() {
    return impl_->latch(AuthorityError::ObservationLost);
}

SourcePermitResult SourceAuthority::permit(AuthorityToken owner,
    AuthorityToken command, std::uint32_t source_offset,
    std::uint32_t source_length, std::uint32_t command_offset,
    std::uint32_t command_length, const SourceCodeIdentity &code) {
    auto &state = *impl_;
    if (state.failure != AuthorityError::None) return {state.failure, {}};
    const auto *owner_record = state.owner(owner);
    const auto *command_record = state.command(command);
    if (owner_record == nullptr || command_record == nullptr)
        return {AuthorityError::StaleToken, {}};
    if (code != state.code || code != state.config.supported_code ||
        owner_record->code != code || !owner_record->binding_valid)
        return {AuthorityError::InvalidIdentity, {}};
    const auto &source = owner_record->source;
    if (!source.active || !source.complete || source.poisoned ||
        source.completed != source.total || source.code != code)
        return {AuthorityError::NotReady, {}};
    if (source_length == 0u || command_length == 0u ||
        std::uint64_t{source_offset} + source_length > source.total ||
        std::uint64_t{command_offset} + command_length > command_record->bytes)
        return {AuthorityError::InvalidRange, {}};
    std::uint32_t source_base{}, source_subbase{}, command_subbase{};
    if (!add_raw(owner_record->base, kSlotOffset, source_base) ||
        !add_raw(source_base, source_offset, source_subbase) ||
        !add_raw(command_record->base, command_offset, command_subbase))
        return {AuthorityError::InvalidRange, {}};
    const auto source_extent = physical_interval(source_base, source.total,
                                                 state.config.ram_bytes);
    const auto source_subrange = physical_interval(source_subbase, source_length,
                                                   state.config.ram_bytes);
    const auto command_subrange = physical_interval(command_subbase, command_length,
                                                    state.config.ram_bytes);
    if (!source_extent || !source_subrange || !command_subrange ||
        !contains(owner_record->slot, *source_extent) ||
        !contains(*source_extent, *source_subrange) ||
        !contains(command_record->allocation, *command_subrange))
        return {AuthorityError::InvalidRange, {}};
    if (overlaps(*source_subrange, *command_subrange))
        return {AuthorityError::Overlap, {}};
    const Interval owner_vptr{owner_record->allocation.begin,
                              owner_record->allocation.begin + 4u};
    if (state.pending_overlaps(*source_extent) ||
        state.pending_overlaps(*command_subrange) ||
        state.pending_overlaps(owner_vptr))
        return {AuthorityError::PendingWriter, {}};
    if (!state.touch()) return {state.failure, {}};
    SourcePermit snapshot{};
    snapshot.token = state.new_token();
    snapshot.owner = owner;
    snapshot.command = command;
    snapshot.load = source.token;
    snapshot.revision = state.revision;
    snapshot.source_base = source_base;
    snapshot.source_bytes = source.total;
    snapshot.source_offset = source_offset;
    snapshot.source_length = source_length;
    snapshot.command_base = command_record->base;
    snapshot.command_bytes = command_record->bytes;
    snapshot.command_offset = command_offset;
    snapshot.command_length = command_length;
    snapshot.resource_id = source.resource_id;
    snapshot.group = source.group;
    snapshot.digest = source.digest;
    snapshot.code = code;
    state.saved_permit = snapshot;
    state.has_permit = true;
    return {AuthorityError::None, snapshot};
}

AuthorityError SourceAuthority::revalidate(const SourcePermit &permit,
    const SourceCodeIdentity &code) const noexcept {
    const auto &state = *impl_;
    if (state.failure != AuthorityError::None) return state.failure;
    if (!state.has_permit || !(permit == state.saved_permit))
        return AuthorityError::InvalidIdentity;
    if (permit.revision != state.revision ||
        code != state.code || code != state.config.supported_code ||
        permit.code != code)
        return AuthorityError::StaleToken;
    const auto *owner = state.owner(permit.owner);
    const auto *command = state.command(permit.command);
    const auto source_extent = physical_interval(permit.source_base,
        permit.source_bytes, state.config.ram_bytes);
    if (owner == nullptr || command == nullptr ||
        !owner->binding_valid || !owner->source.active ||
        !owner->source.complete ||
        owner->source.poisoned || owner->source.token != permit.load ||
        !source_extent || state.pending_overlaps(*source_extent))
        return AuthorityError::StaleToken;
    std::uint32_t command_subbase{};
    if (!add_raw(permit.command_base, permit.command_offset, command_subbase))
        return AuthorityError::StaleToken;
    const auto command_subrange = physical_interval(command_subbase,
        permit.command_length, state.config.ram_bytes);
    if (!command_subrange || state.pending_overlaps(*command_subrange))
        return AuthorityError::PendingWriter;
    const Interval owner_vptr{owner->allocation.begin,
                              owner->allocation.begin + 4u};
    if (state.pending_overlaps(owner_vptr))
        return AuthorityError::PendingWriter;
    return AuthorityError::None;
}

} // namespace mhp3rd::resources
