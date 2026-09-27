# Selected texture slot: asynchronous completion contract (ASSET-012)

This audit follows the main-ELF resource manager after the selected
`0x0896FBC8` owner's selector-7 wrapper submits its destination at
`owner+0x27C70`. The provider gives this slot `0x5800` bytes. It establishes
where a successful read, copy, transform, and request retirement can be
correlated. It does **not** establish a fixed resource ID, a live owner's
loaded bytes, or a format-level logical root length. Queue idle and the
owner's later state 4 remain insufficient evidence of loaded source data.

The source is the supported `NPJB-40001` ELF at
`profiles/mhp3rd/game/EBOOT.ELF`, SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Guest `0x08804000` maps to ELF file offset `0x2CB4`. Generated code was a
control-flow cross-check; the following exclusive-end instruction spans were
hashed again from that ELF. The ignored local
`out/testing/texture-async-worker-static.json` holds the full span and jump
table inventory without original instruction bytes.
Import identities below follow the repository's
`profiles/mhp3rd/analysis/report_imports.csv`: `0x089656A0` is
`sceIoRead`; `0x089658B0`, `0x089657D0`, and `0x08965858` clear, set, and
wait on an event flag; `0x08965890` is a bounded-delay call; and
`0x08965928`/`0x08965908` are data-cache operations.

| Role | Guest span | ELF SHA-256 |
| --- | --- | --- |
| Selected request wrapper | `0x088A5470..0x088A54F4` | `7686a020b89d8ea69d2bbb2e8810f382553eeb4cd49142bf4098e05c7193468c` |
| Queue submission | `0x08863CDC..0x08863F10` | `7d6f35fd8dd75e8e4b61594ad2644f520f03f3549b37fc494eee9253a75f4305` |
| Manager initialization tail | `0x08864504..0x08864680` | `93eb050a972cfd3f399deaad0d083833ca0a3bb6dde6600156141ad8b60d8abf` |
| Reader into scratch | `0x08863608..0x08863664` | `2221e698d6911ea35cdc175db11afad11f7c5f3ea865cb214d7da14391dc1a28` |
| Main read worker | `0x08865450..0x08865D4C` | `0def8f9db4c8760a3187badbc8d6ec5847d8a46322698fa535631aaac5cf1591` |
| Copy worker | `0x088652C4..0x08865378` | `4e7cc4498142063c660fb9e182a5020f1cf767707719510c69c472f363585b92` |
| Transform worker | `0x08865378..0x08865450` | `51f9b498171f9494dd89f00da5b4002e40204ebc524fc26f7679d6ace5905d3c` |
| In-place transform entry | `0x08864BC8..0x08864C54` | `448d8b2abb301a2d64eb915231a0fe88c4642869e743993260608804dbd6b297` |
| In-place transform loop | `0x088639BC..0x08863AF4` | `ffe56d9c7973e4adb1352edd2ffb77baca597efb72e351a876232597538a2f84` |
| Request retirement | `0x08865D8C..0x08865DB4` | `2096eb5cdc659d5eb6a70cc69f1e2e1bebfa70074641a834a9f6c6d84b2c20c4` |
| Group cancellation | `0x08866044..0x088661BC` | `ad270b6b68e05a6de640b05b4495fc142ee8671e82edb8258397aa9564cf7d4e` |

## Request identity and fields

`0x088A5470` passes the mapped 16-bit resource ID, selector-7 destination,
`group=low8(owner[+0x63]+5)`, `t0=0`, and `t1=1` to manager virtual `+0x30`
at `0x08863CDC`. That routine obtains a **requested** length from manager
virtual `+0x14`, queues one or more records, and returns 1 before any read.
It never receives the slot's `0x5800` capacity. A request longer than
`0x20000` is split into chunks; the selected slot must reject any request
whose eventual touched extent exceeds its own capacity, regardless of the
manager's chunk limit.

The queue is 128 records of 32 bytes at `manager+0x8C+32*index`.
`manager+0x108C` is the consumer index, `+0x1090` the producer index, and
`+0x1094` the main worker's control state. The record layout below states
only what the cited instructions establish:

| Record offset | Observed value/use for this wrapper |
| --- | --- |
| `+0x00` | 16-bit active marker set to 1 on enqueue; zeroed on retirement. |
| `+0x02` | Mapped 16-bit resource ID. |
| `+0x04` | Destination returned by selector 7. |
| `+0x08` | This chunk's requested byte count; compared with `sceIoRead` result. |
| `+0x0C` | Destination offset for this chunk. |
| `+0x10` | Original request length, copied into each chunk; not a measured result. |
| `+0x14` | Optional pointer whose target receives 1 on cancellation; zero in the selected wrapper. |
| `+0x18` | 8-bit group supplied by the owner wrapper. |
| `+0x19` | 1 from the wrapper's `t1`; used by postprocessing. |
| `+0x1A` | 0 on earlier split chunks, 1 on the final chunk. |
| `+0x1B` | Byte copied from `manager+0x2F7D4` at enqueue. Normal manager initialization at `0x0886465C` sets that byte to 1. |
| `+0x1C` | Conditional SHA-1 flag selected inside enqueue at `0x08863EB4..0x08863F0C`; it can remain zero while `+0x1B` enables deobfuscation. |

The worker state jump table at `0x0896B12C` sends state 0 to
`0x08865650`, state 7 to `0x0886555C`, state 8 to `0x088654D4`, and state
10 to `0x088655F8`. State 8 is the useful bounded read fixture after a
known open/seek. Its read branch calls `0x08863608`, which invalidates the
manager's `0x20000`-byte scratch buffer at `manager+0x98C0` and tail-calls
`sceIoRead(fd, scratch, record[+8])`. At `0x0886551C` the main worker
compares the actual return value with this chunk's requested count. A short
or negative result enters a delayed retry at `0x08865524` without retiring
the record. A fault can therefore leave the worker retrying indefinitely;
an idle queue or eventual later successful read must not retroactively
certify the earlier attempt. The separate open/seek branch at
`0x08865858..0x088659A0` closes, records manager error state, and retries
after a short or negative read. These are different control paths.

For a complete state-8 read, `0x08865748` advances the manager's observed
source cursor at `+0x298E0` by the returned count. That cursor is a file
position, **not** a decoded output length. If the destination is outside
the classifier's `0x08400000..0x08800000` region, `0x08863664` returns
zero. The main worker then takes `0x088659A4` and calls `0x08865238`
**inline**. After its
address guard, that helper copies exactly `record[+8]` bytes from scratch
to `record[+4]+record[+0xC]`, then writes back the data cache. The separate
copy worker at `0x088652C4` serves destinations inside that region, with
its own event acknowledgment. The selected owner's allocation address is
runtime-dependent; the inline route is proven for the proposed
`0x09000000` synthetic destination, not for every live owner instance.

On the outside-region route with manager-initialized
`record[+0x1B]=1`, the main worker has stored the record pointer at
`manager+0x2F7D0` at `0x08865758`. At `0x088659CC` it signals and waits
for the transform worker through event bit `0x10`. Its `0x08864BC8` path seeds the
transform using the resource's sector index and chunk offset, then calls
`0x088639BC` on the destination **in place**. The loop processes four bytes
per iteration, so a positive chunk length `n` touches exactly
`4*ceil(n/4)` destination bytes; zero touches none. The transform returns
no output byte count, and its return value is not checked by its worker.
When `+0x1C=0`, the worker skips `0x088637E0`. With the flag set,
that helper computes SHA-1 over the destination; it is not a decoder or
written-length result. The state-8 route bypasses the original digest comparison
used by the alternate memory-stick-file path. See the
[transfer audit](TEXTURE_TRANSFER_EXTENT.md) for the precise branch distinction.
On this successful route, the main worker calls `0x08865D8C` after the
transform worker signals bit `0x10`. That routine zeros state `+0x1094`
and the active marker, then advances the consumer index. Other main-worker
branches also call it, including an early group-4 abort at `0x08865804`,
so retirement alone proves no success. The remaining record fields are
**not** erased.

The copies and transformation expose no format-level logical root length.
For this selected path, the observable successful read count plus the
original loops do give an exact write/touch bound: each chunk copies `n`
bytes and transforms `round_up_4(n)` bytes at its destination offset. A
consumer may use the original requested length as a consistency check, not
as proof that the read or transform completed. It must still parse and
validate the actual decoded root within that measured bound.

## Error, cancellation, and reuse consequences

The ordinary wrapper's `record[+0x14]` is null. `0x08866044` group
cancellation marks an active request through that pointer only when it is
nonnull, then removes later matching queued records by compaction. For the
selected active request it cannot use that marker. The owner's reset at
`0x088A53C8` calls this group cancellation when its control field is below
3, but reset alone must not be assumed to prevent a late destination write.
The separate full-queue cancellation at `0x08865F00` also writes optional
markers and clears noncurrent entries. Neither path supplies a successful
read count. Any reset, canceled generation, owner teardown/free, or owner
address reuse revokes capture authority, even if bytes or stale descriptor
fields remain in RAM. The main worker's manager-wide error reporting at
`0x08864998` is not a per-record success certificate.

## Capture and finite original-code gate

A safe observer should bind one live owner generation and selector 7 to the
record **at enqueue**: `(owner generation, resource ID, destination,
group, original request length, chunk offset, chunk length, final-chunk
marker)`. Capture each actual `sceIoRead` result at the `0x08863608`
return/`0x0886551C` equality branch. On the outside-region route, match
the same unchanged record after the inline copy, at the transform worker's
event completion, and just
before `0x08865D8C` clears its active marker. For the normal mode, require
all expected chunks to have a positive, exact read result, contiguous offsets,
an observed final chunk, a consistent total, and no intervening reset,
cancel, error, free, or generation change. Validate both
`offset+n <= 0x5800` and `offset+round_up_4(n) <= 0x5800`, with no integer
overflow. The resulting high-water mark is a bound on bytes actually
touched by this transfer; a decoded root parser supplies the smaller
logical source extent if the format has one. A live destination taking the
other classifier route needs separate copy-worker event evidence; the
outside-region gate cannot certify it. A pending token or queue-idle
observation cannot replace these edges.

A finite synthetic original-code gate can start the main worker at state 8
with one constructed record, outside-region destination `0x09000000`, a
modeled `sceIoRead` result, and a bounded
event-flag model that runs the **original** transform worker body on its
signal. The main worker itself executes the original inline copy helper.
The gate should stop after one `0x08865D8C` retirement and verify
CPU/RAM equality against the independent interpreter, including scratch,
destination, record marker, and cursor. Cases needed are exact read;
short/negative read with one bounded retry and no retirement; final-chunk
extent at the `0x5800` boundary; cancellation before and during the active
request; and address reuse after reset. Stub only imported I/O and scheduler
boundaries. Do not substitute the original copy, transform, or retirement
logic. This document's local JSON is static evidence; that execution gate
and a real selected-owner request correlation were not run by this audit.
