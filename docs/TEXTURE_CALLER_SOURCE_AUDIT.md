# Texture builder caller source audit (ASSET-012)

This is a bounded, read-only audit of one original call path to the texture
command builder. It establishes a concrete source-slot address and a candidate
fixed capacity for one virtual resource provider. It does **not** establish
that a live game object was allocated, populated, ready, or still owned at a
particular builder call. No game run, production hook, or new manual case was
used. The ignored local evidence file
`out/testing/texture-caller-source-audit.json` contains only addresses,
lengths, and fingerprints. A separate bounded original-code gate has since
executed this provider and caller path with a constructed owner; it does not
establish live-game ownership.

The supported `NPJB-40001` ELF is `profiles/mhp3rd/game/EBOOT.ELF`, SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Its first `LOAD` segment maps guest address `0x08804000` to file offset
`0x2CB4`. All ranges below have exclusive ends. The generated C++ labels were
used to cross-check control flow; the fingerprints were taken from the ELF.
No original instructions or resource bytes are copied into this document.

## Selected path and observed bounds

| Guest span | ELF file span | SHA-256 | Observation |
| --- | --- | --- | --- |
| `0x088B0398..0x088B0400` (104 bytes) | `0xAF04C..0xAF0B4` | `19eedc1438572a78724b5b7e432c2782dd4a2a2a384f17cb12dbf682d23f2e50` | The caller gets a root from virtual slot `+120` with selector 7, requests child 2 through `0x088661C8`, allocates `36 × child[+8]` bytes aligned to 16 via `0x08879DB4`, and calls builder `0x0889E5C0` with state `object+0x13F0`, destination allocation result, and source child pointer. This bounded window includes the builder call delay slot but is not the full caller function. |
| `0x0896FBC8..0x0896FC44` (124 bytes) | `0x16E87C..0x16E8F8` | `9c6c7c981b64ac8bff0437940f65740ad9503a7c84ab96c8d8013e0f0e903a54` | One candidate object vtable prefix; its slot `+120` at `0x0896FC40` points to `0x088B7DE0`. The caller must check this exact vptr and target before using this provider contract. |
| `0x088B7DE0..0x088B7E04` (36 bytes, including return delay) | `0xB6A94..0xB6AB8` | `d17da887d7cd999fae774f22c747885ed2ef90be06cf0bd3909bf9f34d5e7fdd` | With object in `a0` and selector in `a1`, reads one 32-bit offset from `0x089E8834 + 4 × selector` and returns `object + 0x1470 + offset`. It does not read an object field or check selector bounds. |
| `0x089E8834..0x089E8858` (nine words) | `0x1E74E8..0x1E750C` | `6b3630c3cd2c5e41756d6048bef85abe930f7ddf93796b5a2bb9f7222b98cba7` | Offset table: selector 7 is `0x26800`, selector 8 is `0x2C000`. |
| `0x089E8810..0x089E8834` (nine words) | `0x1E74C4..0x1E74E8` | `1d921ef9af87716c9b6dcc60a338ca3a14b05edae91c46e8e8c3c132bf163b2a` | Adjacent slot-size table: selector 7 is `0x5800`, equal to the difference between offsets 8 and 7. |
| `0x088A5324..0x088A53C8` (164 bytes) | `0xA3FD8..0xA407C` | `8ee3fd94e01c70f5d6d627f11fecf0c885efca84300823b144c8c32dc953c98f` | A separate copy path resolves the same destination slot through virtual slot `+120`, reads this size table by selector, and copies that many bytes from its caller-supplied source pointer. This supports the table's role as a fixed slot capacity; it does not prove the selected object took this copy path. |

For this vtable and selector 7, the provider returns `object + 0x27C70`.
The fixed slot end is `object + 0x2D470`; the nine-slot table ends at
`object + 0x2F470`. These are object-relative offsets, not standalone heap
allocations. The provider itself supplies no length. A live allocation that
contains the object and the selected slot is still required. The fixed table
capacity is independent of the texture child's record count at `child+8`.

The indexed accessor `0x088661C8..0x08866234` (full-span SHA-256
`eb001e25ea896978ea3ce1a330e39005bc9db9e4d83ac44781bd86cf755957a8`)
returns `root + root[+0x14]` for child index 2 when the root count permits it
and the offset is nonzero. It does not return or check the child length.
The adjacent accessor `0x08866234` can read the indexed length at
`root+0x18`, but this caller does not invoke it. A native boundary may use
that index pair only after checking the entire table and `offset + length`
against an independently established live root-slot extent. The pair is
untrusted format metadata, not allocation authority. The builder's own
size-prefixed records must then fit within the checked child slice.

The local decoded-entry corpus verifies examples of this indexed structure
and the TMH layout, including child spans, in
[RESOURCE_BUNDLE_CONTRACT.md](RESOURCE_BUNDLE_CONTRACT.md) and
[TMH_RESOURCE_CONTRACT.md](TMH_RESOURCE_CONTRACT.md). No source ID has been
observed flowing through this specific virtual provider and caller, so those
samples do not establish this caller's live resource identity.

## Ownership and invalidation evidence

The vtable above has teardown-like entries at `0x088BA6E4` and
`0x088BA74C`. Both write the same vptr during teardown and can enter the
base routine `0x088B1978`, which changes the object's vptr and tears down
subobjects. These are observable invalidation boundaries for a resource-slot
claim. A third routine at `0x088BA67C` also writes this vptr and can enter
the same base teardown; it must not be treated as a certified constructor.
The visible tail at `0x088BA7A4` reaches `0x088A0E1C`, which only returns;
it does not establish an object free.

One separate main-ELF path at `0x088BD058..0x088BD0A4` requests and clears
`0x2F470` bytes, matching the full table end above, then later calls an
overlay address. The visible code does not link that allocation or overlay
constructor to vtable `0x0896FBC8`. Its matching size is a lead for the next
audit, not owner provenance. No direct call to `0x088BA67C` appears in the
generated main-ELF corpus. The actual creation, load completion/error, free,
and in-place reuse edges for this vtable remain unresolved.

Other vtables containing caller method `0x088B0114` have different slot
`+120` targets: `0x0896FA78` returns zero through `0x088A5318`, while
`0x0896FD18` uses `0x088B7E98` and another offset table, and `0x0896FED8`
uses `0x088F41E0` with the shared offset table but a different object base.
The selected contract must not silently admit those classes or other builder
callers.

## Checked production authority, if the remaining links are proved

For builder return address `0x088B0400`, the dispatch could derive the object
from the caller-preserved `s8` register, check `a0 == object + 0x13F0`, and
require vptr `0x0896FBC8` with slot `+120 == 0x088B7DE0`. It would also need
an allocation registry record whose **exact object base**, live allocation
identity, generation and byte extent contain at least
`[object+0x27C70, object+0x2D470)`. A mapped RAM page or coincidental
containing allocation is insufficient. The record must come from the actual
object creation path, and its ready state must come from a successful source
load or copy completion with a validated byte count. A failure, reset,
partial reload, teardown, free, or in-place reconstruction revokes the state;
address reuse creates a new generation.

While that generation is held stable for one builder call, read the root's
count and child-2 pair inside the live slot; reject an absent child, invalid
table, overflow, zero or out-of-slot length, and require the resulting child
pointer to equal builder register `a2`. Pass the bounded child length to the
existing adapter only after its metadata traversal and disjointness preflight
succeed. Recheck the destination allocation independently. Any failed or
unknown authority check leaves the original builder path intact, with no
guest mutation by the adapter. Record attempted, accepted and fallback counts
for this exact return address and vtable; do not count unrelated builders.
Baseline remains on original execution. No producer, registry, concurrency
exclusion, or production dispatch is installed by this audit.

## Bounded original execution and next verification

The local gate `out/testing/texture-allocation-provenance-gate.json` passed
168 original AOT/interpreter calls across its allocator, free and caller
paths. Sixteen caller cases include eight that execute virtual provider
`0x088B7DE0` with selector 7 and vtable `0x0896FBC8`, then the original
indexed child lookup, command allocation, builder and epilogue. Full RAM,
VRAM and CPU contexts matched; the largest interpreter case took 18,076
bounded slices. The owner and source inputs were constructed, and no actual
object constructor or resource loader was executed. These results certify
the selected instruction and register flow, not live ownership.

To close the source authority gap, trace the actual vtable assignment and
owning allocation through the relevant overlay or object factory, then
identify the loader's exact write length, success/ready flag, reset and
release paths. Any class or source slot whose owner or load state cannot be
tied to that generation stays on the original builder.
