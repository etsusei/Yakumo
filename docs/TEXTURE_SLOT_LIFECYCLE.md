# Selected texture slot lifecycle (ASSET-012)

This is a static, bounded audit of the selected `0x0896FBC8` owner and its
selector-7 source slot. It follows the main ELF's request, polling, build,
reset, and command-release edges. It does not certify that a particular live
resource has finished loading, that its decoded bytes fit the slot, or that
the builder is always called only after successful loading. No game run or
production hook was used for this audit.

The supported `NPJB-40001` ELF is `profiles/mhp3rd/game/EBOOT.ELF`, SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Its file-backed load segment maps guest `0x08804000` to file `0x2CB4`.
Generated code supplied control-flow cross-checks; instruction fingerprints
below were recomputed from the ELF. Ranges have exclusive ends and include
return delay slots when they name a complete routine. The ignored local
`out/testing/texture-slot-lifecycle-static.json` records all cited spans and
vtable entries without original bytes. Local disassembly is likewise ignored.

| Role | Guest span | ELF SHA-256 |
| --- | --- | --- |
| Selected vtable, 84 words | `0x0896FBC8..0x0896FD18` | `fad6042cf41b164b9a98a2238ca4146113f94e637b760b04bd4362e9a6448af6` |
| Normal request wrapper | `0x088A5470..0x088A54F4` | `7686a020b89d8ea69d2bbb2e8810f382553eeb4cd49142bf4098e05c7193468c` |
| Normal poller | `0x088A5564..0x088A570C` | `19a511eb54698ecde00538f5b4b93b72f528dae1039506c3596bbed923994271` |
| Alternate poller | `0x088A570C..0x088A58B8` | `2ef9ab50736978c4cd98ff6374c59ef8c6001f84649a5f0b07aa4d992612c497` |
| Resource-ID mapper | `0x088A58DC..0x088A5A80` | `3506935fe01bf28fbc38d542dacfb19a390d8dc169e967d5a78b57fd21b3065b` |
| Build driver | `0x088A5A80..0x088A5B20` | `c428325879f49adae7ed4562340b48749e62f1445cd143dff04442169e285664` |
| Manager request enqueue | `0x08863CDC..0x08863F10` | `7d6f35fd8dd75e8e4b61594ad2644f520f03f3549b37fc494eee9253a75f4305` |
| Manager request length | `0x08864E8C..0x08864F0C` | `2e38e03648cbe0def71ceb1adc68372c198a25d491d3af76f854d9957cfa4a82` |
| Normal reset | `0x088A53C8..0x088A5420` | `17538afb0786b0651e84a0ea39ed4fd3e9f7e0e08382b77e2ba565fb999f4e0f` |
| Selected command release | `0x088A3474..0x088A3588` | `5f0bc1f6c503a20292d26a8c7c30e43797a7e10528f384998e790ac6ab872a37` |
| Container release candidate | `0x088BA9D8..0x088BAAF0` | `09b88e3ea930e9363b0d2e8ec2f301bb49549d4320b49637dfab1d2efe9e9868` |

## Selected dispatch and request

The selected vtable's `+0x78` provider is `0x088B7DE0`, so selector 7
returns `owner+0x27C70`; the fixed table gives this slot `0x5800` bytes and
an end at `owner+0x2D470`. The vtable's `+0x7C` method at `0x089613C4`
returns 9. Its `+0x90/+0x94/+0x98/+0x9C` entries are, respectively,
`0x088A5470`, `0x088A5564`, `0x088A570C`, and `0x088A54F4`.
`+0xA4/+0xA8/+0xAC` are `0x088A58B8`, `0x088A58DC`, and
`0x088A5A80`; `+0xB0/+0xB4` are `0x088B1B64` and the selected builder
caller `0x088B0114`. These are exact table entries, not inferred class names.

The normal poller's state-1 branch asks virtual `+0xA8` to map its current
index to a 16-bit resource ID. For index 7, `0x088A58DC` calls
`0x0886A534` using `owner+0x60`, then uses `owner+0x62` and tables at
`0x089CD1F8/0x089CD22C` to clamp and offset that ID. This is a runtime
mapping; no fixed source entry ID is established. If the mapped ID differs
from `owner+0xB8C+4*index`, the poller calls virtual `+0x90` with the index
and mapped ID. For the selected vtable, `0x088A5470` obtains the destination
through virtual `+0x78` and submits `(manager, resource_ID, destination,
group, 0, 1)` to manager virtual `+0x30`, where
`group = low8(owner[+0x63] + 5)`. The main-ELF initialization at
`0x0888D930..0x0888D950` installs the manager pointer in global
`0x08A3A03C`; `0x08865D4C` gives that manager vtable `0x0896F648`,
whose `+0x30` target is `0x08863CDC`.

`0x08863CDC` obtains a request length through manager virtual `+0x14`
(`0x08864E8C`) and queues the destination, ID, length, and group. It returns
1 after enqueue, before the asynchronous worker transfers bytes. For IDs
found in its 0x509-entry lookup, `+0x14` returns a stored 32-bit length;
otherwise it computes a sector-index difference times `0x800`. The queue
splits requests above `0x20000` bytes into chunks. This is a **requested
transfer length**, not a validated decoded root length or the selected
slot's fixed capacity. The manager does not receive `0x5800` from this
wrapper and the examined request path has no destination-capacity check.
The worker branches through read/copy or transform routines, including
`0x08863608`, `0x088637E0`, and `0x08864BC8`. Its successful decoded output
extent for this selected slot has not been established.

The adjacent alternate wrapper at `0x088A54F4` submits to the same manager
and provider but uses group 10. Its `+0x98` poller uses the same object state
and per-index table while polling manager virtual `+0x3C` for group 10.
The normal poller uses manager `+0x34`. Which poller the selected owner's
overlay actually invokes is a separate call-edge question; their shared
state must not be counted as two independent successful loads.

## What the state fields prove

The object's `+0xB80` is an observed control value, without a certified
gameplay-ready meaning:

| Observed value | Normal `+0x94` poller action |
| --- | --- |
| 0 | If virtual provider `+0x78` for selector 0 is nonzero, write 1 to `+0xB80` and zero `+0xB84/+0xB88`. |
| 1 | Map the resource ID for `+0xB84`; submit through `+0x90` only when it differs from the cached `+0xB8C` entry; write 2 to `+0xB80` regardless of the submission return. |
| 2 | Poll the global manager; on its idle result, copy `+0xB88` into the cached entry, increment `+0xB84`, and return to 1 for another index or write 3 after the ninth index. |
| 3 or another value | No further request or completion validation in this poller. |

The alternate `+0x98` poller has the same progression with `+0x9C` and the
group-10 manager poll. The cache is a resource-ID comparison, not a byte
fingerprint. A cached match skips submission without checking that slot
contents still belong to the current object generation. The manager poll
indicates its queue/worker state; neither poller compares a per-request
returned byte count with `0x5800`, checks a decoded root header, or verifies
the selected slot after transfer. A failed or canceled request and later
idle manager state therefore cannot be promoted to a successful captured
source by these fields alone.

The selected `+0xAC` driver at `0x088A5A80` first invokes `+0xA4`.
`0x088A58B8` reports true when `owner+0xB80 >= 4`. If false, the driver
calls `+0xB0` with selectors 0 through 6, then calls `+0xB4` once for the
selector-7 texture path, calls `0x088A58CC` to write 4 to `+0xB80`, and
writes `0x7F` to `owner+0x141C`. Its own gate is only “less than 4”: it does
**not** require the poller to have reached 3. Thus this edge establishes the
builder's place in the static state flow, but no unconditional guarantee that
the slot was fully loaded before building. A caller-side overlay gate may
provide an additional precondition; that edge is not certified here.

## Reset, release, and address reuse

The complete normal reset at `0x088A53C8` tests `owner+0xB80 < 3`; in that
case it calls manager `0x08866044` for group
`low8(owner[+0x63] + 5)`. It then zeros `owner+0xB84` and `+0xB80` in
either case. It does not clear the slot bytes or the cached `+0xB8C` IDs.
Consequently reset is an invalidation event even if the old root header still
looks parseable. `0x088A5420` is the corresponding group-10 reset. These
reset functions alone do not release the command pointer or owner allocation.

The selected builder at `0x088B03C4..0x088B0400` requests
`36 * child[+8]` command bytes from the manager in global `0x09FBE75C`,
calls `0x0889E5C0` with state `owner+0x13F0`, and thereby stores the returned
command pointer at `owner+0x13F4`. On the container release path
`0x088BA9D8`, a nonnull owner is first passed to `0x088A53C8`, then to
`0x088A3138` and `0x088A3474` with that same global command manager. The
last routine reads `owner+0x13F4`, calls `0x08879FF0` on the nonnull pointer
at `0x088A3530`, clears `+0x13F4`, and only then calls `0x0889D6D0` to zero
the 16-byte state. Its branch at `0x088A34D0` may adjust manager metadata,
but rejoins before this free. `0x088A3138` frees the analogous pointers in
the seven lower state records; its loop does not include `+0x13F0`.
Merely observing `0x0889D6D0` in an initializer is **not** evidence of a
free; this release edge supplies the missing direct allocator call.

After command cleanup, `0x088BA9D8` invokes the owner's virtual `+4`
destructor. It then searches the registry rooted at global `0x09FBE8D8`:
the count is at `container+0xB43D30`, owner pointers at
`container+0xB43134+12*i`, and the manager for `0x08879FF0` is
`container+4`. On a pointer match it frees the owner and marks the matching
record's key at `container+0xB43130+12*i` as `-1`. The selected vtable's
`+0/+4` teardown targets `0x088BA6E4/0x088BA74C` set the vptr and enter
`0x088B1978`, which changes the vptr again and tears down subobjects. Both
the reset and the vptr change revoke source/command authority; the later
allocator free permits address reuse. The caller's `container+8+4*index`
slot must be tied to the selected owner by the separate creation/container
trace before treating this release entry as an observed selected-instance
path.

`0x088A5324` remains a counterexample to an easy but unsupported copy claim:
it copies a fixed slot-size table entry from a caller-supplied source, but
its address is absent from all 84 entries of the selected vtable and no
direct main-ELF call from this selected load chain was found. The selected
`+0x90` wrapper submits through the asynchronous manager instead.

## Authority still needed

The request path gives a concrete place to observe `(exact owner generation,
selector 7, resource ID, destination=owner+0x27C70, requested length)`.
A production authority still needs the actual worker completion edge with
successful result and exact decoded destination extent, bound against the
live `0x2F470` owner allocation and the `0x5800` slot. It also needs the
overlay caller edge proving that the `+0xAC` build follows that success for
the same generation, plus interception of reset, cancel, teardown, command
free, owner free, and in-place reuse. Until those links are present, state 3,
state 4, a mapped RAM span, a cached ID, or a parseable child table does not
authorize the native builder adapter. No new manual gameplay case is needed
for this remaining static/instrumented proof.


The subsequent [transfer completion gate](TEXTURE_TRANSFER_VALIDATION.md) and
[source-authority contract](TEXTURE_SOURCE_AUTHORITY.md) resolve the bounded
DATA.BIN completion/write-extent investigation. ASSET-012 is complete in that
scope; ASSET-013 implements the authority next. These later results do not
retroactively claim live loading, actual concurrent scheduling, alternate-file
coverage or production integration for this audit.
