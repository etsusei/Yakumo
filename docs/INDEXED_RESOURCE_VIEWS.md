# Portable indexed resource views (ASSET-002)

The original resource accessors operate on a parent pointer in PSP memory.
`host/resources/indexed_bundle.hpp` exposes that recovered index as a portable
C++20 API with no Runtime, GuestMemory, CPU context or renderer dependency.
It is a new boundary for future native resource consumers; the game still
uses its original loaders and objects. No default runtime hook is installed.

The byte contract and original caller evidence are recorded in
[RESOURCE_BUNDLE_CONTRACT.md](RESOURCE_BUNDLE_CONTRACT.md). This reader does not
claim that every structural match is a real bundle, or that a child marker
identifies a fully understood model, animation or texture format.

## API and storage ownership

`SharedBytes::take` accepts a vector by value and retains it as shared immutable
storage. `slice` creates a checked view sharing that storage. `bytes()` returns
a non-owning const span; keep its `SharedBytes` value alive while using it.
`IndexedBundle::parse` owns a parent slice and copied index metadata. A returned
child remains valid after the bundle, its parent handle, or an intermediate
nested view is destroyed. This is required by the observed original consumers,
which retain child pointers in longer-lived objects.

The complete parent, including gaps and nonzero tails, is retained. Child
offsets are relative to the decoded parent slice. Slicing a nested child does
not reinterpret that offset as an ISO address or lose the backing allocation.
No payload is copied when returning a child or parsing a nested slice.

| Query | Result |
| --- | --- |
| In-range nonzero offset | Checked child slice, including an engaged empty slice for zero length |
| In-range zero offset | Absent child (`nullopt`); advertised length remains in the record |
| Index outside the table | `out_of_range`, distinct from an absent slot |
| Malformed header/table/span or exceeded count budget | `invalid_argument` |

Counts are limited to the original nonnegative signed-index domain and a
caller-supplied finite budget (4,096 by default). The table is bounded before
allocating its entries. Present children must start after the table and fit
inside the parent using overflow-safe subtraction. Unsorted, overlapping and
aliased spans are accepted. No alignment, magic, zero padding or monotonic
offset rule is invented. The native API deliberately refuses unsafe malformed
accesses rather than reproducing the original routines' negative-index hazard.

## Independent original-code gate

`mhp3rd_indexed_bundle_oracle` links the existing production AOT objects and
requires the registered full ELF identity plus all three complete consumer
fingerprints. For each candidate node it calls the original count accessor,
both accessors for every indexed slot, and both out-of-range results. An
independent bounded interpreter executes the same routines with randomized
initial CPU state. Full CPU state, stack writes/canaries and unchanged index
bytes must agree. Each interpreter call is bounded to 64 slices and the known
consumer address span; the AOT returns to an external sentinel.

Only count/index bytes are presented to these original accessors. Returned
payload addresses are compared as scalar values; no original child decoder,
allocator, resource object or game loop is run. Native child spans and hashes
are checked against the actual file data independently of those scalar queries.

The asset-free core test has 24 synthetic checks, including truncation, budgets,
absent slots with advertised lengths, empty present slices, overlap, aliasing,
tails, and lifetime after parent destruction. Both strict Clang and
AddressSanitizer/UndefinedBehaviorSanitizer runs passed on Apple Silicon macOS.

## Corpus inventory and reproducibility

Build through CMake:

```sh
cmake --build out/mhp3rd --target mhp3rd_indexed_bundle_tests mhp3rd_indexed_bundle_oracle -j2
ctest --test-dir out/mhp3rd -R '^mhp3rd_indexed_bundle_tests$' --output-on-failure
python3 profiles/mhp3rd/tools/inspect_resource_bundles.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_indexed_bundle_oracle \
  --output out/testing/new-bundle-audit.json
```

The tool validates sequential source IDs and bounded lengths from the prepared
manifest. Every raw entry is rehashed before classification. It records each
source entry, each candidate child offset/advertised length/hash, absent slots,
nested parent links, unindexed tails and explicit classification confidence.
The source manifest digest binds original entry/ISO mappings. Child coordinate
spaces remain decoded-parent-relative. Zero-count files are left unclassified
by this survey even though the explicit API supports empty bundles.

The audit is limited to 60 seconds, 256 MiB per parent, depth two, 10,000
candidate nodes, 200,000 child slots and 2 GiB of child hashing. It publishes a
new metadata report only after the complete indexed corpus passes. Existing
reports and raw resources cannot be overwritten. It neither exports payload
copies nor changes the runtime's ISO source.

All 6,043 entry identities were checked. The measured corpus contains 2,168
top-level structural candidates and 28 nested candidates, comprising 9,622
slots, including 555 absent slots. There are 2,185 nodes with a `pmo` or `TMH`
child-marker observation and 11 nodes supported only by bounds. For example,
entries 5202 and 5409 remain weak matches; entry 4107 is a bounds-only outer
node containing nested signature-bearing candidates. These categories must not
be promoted into semantic identification. The original AOT and interpreter each
completed 25,832 queries with no scalar, CPU or memory discrepancy.

The next useful step is a separately evidenced child-format contract. The
original source-ID-to-caller edge, child internal references and transforms,
complete game-object ownership and live rendering/animation behavior remain
unresolved. Existing native-data and vector-discovery test apps are unchanged.


Implementation `903b0f3` is published in draft PR
[#20](https://github.com/etsusei/Yakumo/pull/20). Final local evidence is
`out/testing/indexed-resource-bundle-audit-final.json`; source, executable and
production object hashes are in `out/testing/indexed-resource-validation.json`.
The sanitizer log is `out/testing/indexed-bundle-sanitizers.log`.
