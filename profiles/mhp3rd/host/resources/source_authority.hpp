#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mhp3rd::resources {

// Values in this interface are observations supplied by a future original-code
// adapter. This single-threaded metadata class neither reads guest memory nor
// performs guest writes; the adapter must serialize its event calls. Replacing
// an authority instance after observation loss cannot erase an old writer:
// the adapter must prove all old writers quiescent before rebuilding leases.
struct SourceCodeIdentity {
    std::uint64_t module{}, epoch{}, factory{}, caller{}, provider{};
    std::uint64_t copy_worker{}, transform_worker{};
    friend bool operator==(const SourceCodeIdentity &, const SourceCodeIdentity &) = default;
};

struct AuthorityToken {
    std::uint64_t instance{}, serial{};
    friend bool operator==(const AuthorityToken &, const AuthorityToken &) = default;
};

enum class AuthorityError {
    None,
    Retry,
    RecordedIneligible,
    InvalidConfig,
    InvalidIdentity,
    InvalidRange,
    InvalidOrder,
    InvalidReceipt,
    UnknownToken,
    StaleToken,
    NoCapacity,
    CounterExhausted,
    ObservationLost,
    NotReady,
    PendingWriter,
    Overlap,
};

// recorded means the observation was accounted for, including when a stale or
// poisoned request can no longer contribute to a source. A failed receipt is
// never silently treated as successful transfer evidence.
struct AuthorityEvent {
    AuthorityError error{AuthorityError::None};
    bool recorded{false};
    [[nodiscard]] bool ok() const noexcept { return error == AuthorityError::None; }
};

struct AuthorityIssue {
    AuthorityEvent event{};
    AuthorityToken token{};
};

enum class SourceRoute { DataBinState8, Unsupported };
enum class SourceCopyMode { Inline, Worker };
enum class SourceDigestEvidence { NotChecked, ComputedOnly, ReferenceCompared };

struct SourceAuthorityConfig {
    std::uint32_t ram_bytes{32u * 1024u * 1024u};
    std::uint32_t owner_allocator{}, command_allocator{};
    SourceCodeIdentity supported_code{};
    std::size_t max_allocations{32}, max_requests{64};
    std::size_t max_descriptors{64}, max_external_writers{32};
    std::uint64_t serial_ceiling{UINT64_MAX};
};

struct SourceLoad {
    AuthorityToken owner{};
    std::uint32_t resource_id{}, group{}, total_bytes{};
    SourceRoute route{SourceRoute::DataBinState8};
    SourceCodeIdentity code{};
};

struct SourceFragment {
    AuthorityToken load{}, descriptor{};
    // destination is the slot base; the observed copy/transform address is
    // destination + offset, checked in both raw and physical address spaces.
    std::uint32_t destination{}, resource_id{}, group{};
    std::uint32_t offset{}, bytes{}, total_bytes{};
    bool first{}, last{}, deobfuscate{}, hash_flag{};
    SourceCopyMode copy_mode{SourceCopyMode::Inline};
    SourceCodeIdentity code{};
    friend bool operator==(const SourceFragment &, const SourceFragment &) = default;
};

struct SourcePermit {
    AuthorityToken token{}, owner{}, command{}, load{};
    std::uint64_t revision{};
    std::uint32_t source_base{}, source_bytes{};
    std::uint32_t source_offset{}, source_length{};
    std::uint32_t command_base{}, command_bytes{};
    std::uint32_t command_offset{}, command_length{};
    std::uint32_t resource_id{}, group{};
    SourceDigestEvidence digest{SourceDigestEvidence::NotChecked};
    SourceCodeIdentity code{};
    friend bool operator==(const SourcePermit &, const SourcePermit &) = default;
};

struct SourcePermitResult {
    AuthorityError error{AuthorityError::NotReady};
    SourcePermit permit{};
    [[nodiscard]] bool ok() const noexcept { return error == AuthorityError::None; }
};

class SourceAuthority {
public:
    static constexpr std::uint32_t kOwnerBytes = 0x2F470u;
    static constexpr std::uint32_t kSlotOffset = 0x27C70u;
    static constexpr std::uint32_t kSlotBytes = 0x5800u;
    static constexpr std::uint32_t kLobbyVtable = 0x0896FBC8u;
    static constexpr std::uint32_t kPhysicalMask = 0x1FFFFFFFu;

    explicit SourceAuthority(const SourceAuthorityConfig &config);
    ~SourceAuthority();
    SourceAuthority(const SourceAuthority &) = delete;
    SourceAuthority &operator=(const SourceAuthority &) = delete;
    SourceAuthority(SourceAuthority &&) = delete;
    SourceAuthority &operator=(SourceAuthority &&) = delete;

    [[nodiscard]] AuthorityError failure() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

    // The adapter must attest the exact owner factory/constructor chain and
    // vtable; a matching address or vtable found by a scan is insufficient.
    [[nodiscard]] AuthorityIssue construct_owner(std::uint32_t base,
        std::uint32_t requested_bytes, std::uint32_t allocator,
        std::uint32_t constructed_vtable, const SourceCodeIdentity &code);
    [[nodiscard]] AuthorityEvent reset_owner(AuthorityToken owner);
    [[nodiscard]] AuthorityEvent release_owner(AuthorityToken owner);
    [[nodiscard]] AuthorityIssue allocate_command(std::uint32_t base,
        std::uint32_t requested_bytes, std::uint32_t allocator);
    [[nodiscard]] AuthorityEvent release_command(AuthorityToken command);
    // Code epochs are monotonic; a repeated/regressive change is lost evidence.
    [[nodiscard]] AuthorityEvent code_changed(const SourceCodeIdentity &code);

    // A descriptor ring slot receives a fresh token on every observed reuse.
    // Canonical cached/uncached aliases share one bounded history entry.
    [[nodiscard]] AuthorityIssue begin_descriptor(std::uint32_t raw_address,
        std::uint32_t descriptor_bytes = 32u);
    // An observed unsupported or malformed load start latches observation
    // loss because its future writes have no tracked request destination.
    // An adapter with a known unsupported writer footprint must register it
    // through begin_external_write and prove its quiescence separately.
    [[nodiscard]] AuthorityIssue begin_load(const SourceLoad &load);
    [[nodiscard]] AuthorityIssue begin_fragment(const SourceFragment &fragment);
    // Freshly sample every execution-affecting descriptor field before each
    // read/copy/transform/postprocess phase and before a successful terminal.
    // A mismatch poisons the load but the known request retains its hazard.
    [[nodiscard]] AuthorityEvent observe_descriptor(AuthorityToken request,
        const SourceFragment &fresh);
    // Call only after the actual original operation completes. Worker mode
    // additionally requires a matched worker operation and acknowledgement;
    // a signal by itself is not a copy or transform receipt.
    [[nodiscard]] AuthorityEvent observe_read(AuthorityToken request,
        std::int64_t returned_bytes);
    [[nodiscard]] AuthorityEvent observe_copy(AuthorityToken request,
        std::uint32_t raw_destination, std::uint32_t bytes,
        SourceCopyMode observed_mode);
    [[nodiscard]] AuthorityEvent observe_transform(AuthorityToken request,
        std::uint32_t raw_destination, std::uint32_t footprint_bytes,
        bool digest_computed);
    [[nodiscard]] AuthorityEvent observe_postprocess(AuthorityToken request);
    // `successful` means the original success route was observed. Even that
    // route retires a hazard only after every matched receipt and final fresh
    // descriptor snapshot. Failure/incomplete terminal observations and
    // cancellation leave the hazard pending until proven quiescence.
    [[nodiscard]] AuthorityEvent observe_terminal(AuthorityToken request,
        bool successful);
    [[nodiscard]] AuthorityEvent cancel_request(AuthorityToken request);
    [[nodiscard]] AuthorityEvent prove_request_quiescent(AuthorityToken request);

    [[nodiscard]] AuthorityIssue begin_external_write(std::uint32_t raw_destination,
        std::uint32_t bytes);
    [[nodiscard]] AuthorityEvent observe_external_write(AuthorityToken writer,
        std::uint32_t raw_destination, std::uint32_t bytes);
    [[nodiscard]] AuthorityEvent prove_external_quiescent(AuthorityToken writer);
    // An instantaneous completed write with a known actual span. If an
    // unidentified writer may write again, report observer_lost instead.
    [[nodiscard]] AuthorityEvent observe_unknown_write(std::uint32_t raw_destination,
        std::uint32_t bytes);
    [[nodiscard]] AuthorityEvent observer_lost();

    // Positive command-output ranges only; the original no-command builder
    // domain remains outside this offline permit API.
    // The caller must separately validate resource structure, prove stable
    // execution context, and exclude all relevant guest writers through
    // revalidation and native commit. A permit is only an authority snapshot.
    [[nodiscard]] SourcePermitResult permit(AuthorityToken owner,
        AuthorityToken command, std::uint32_t source_offset,
        std::uint32_t source_length, std::uint32_t command_offset,
        std::uint32_t command_length, const SourceCodeIdentity &code);
    [[nodiscard]] AuthorityError revalidate(const SourcePermit &permit,
        const SourceCodeIdentity &code) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mhp3rd::resources
