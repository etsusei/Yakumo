# Texture command allocation provenance (ASSET-012)

This is a bounded audit of the allocator used by the original texture-command
caller. It supplies facts for a future allocation-bound authority; it does not
install a production hook or establish the lifetime of every texture resource.
The caller's source-resource ownership is separate from the command allocation
covered here. Addresses, formulas, and structure offsets below apply to the
supported NPJB-40001 executable only.

## Evidence and spans

The local `profiles/mhp3rd/game/EBOOT.ELF` is little-endian MIPS ELF32 with
SHA-256 `55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Its file-backed load segment starts at guest `0x08804000`, file `0x2CB4`.
Generated code was used to trace control flow, with each listed instruction
span checked against the ELF. All ends are exclusive and include return delay
slots. The ignored, metadata-only local record is
`out/testing/texture-allocation-static-contract.json`.

| Role | Guest span | ELF file span | SHA-256 | Scope |
| --- | --- | --- | --- | --- |
| Reset node/list | `0x08879D58..0x08879DA4` | `0x78A0C..0x78A58` | `894e42fdfb9b2f533046a30667aab34a39d3b96e885bbe4921836ad5c79906eb` | Complete reset entry through return. |
| Initialize manager prefix | `0x08879DA4..0x08879DB4` | `0x78A58..0x78A68` | `4d29df6ecb3632c2dd1bbfbffeea70270c33ec6a83c926ba35489d7b76de1cc7` | Tail-branches into the reset span above; **both** spans make the complete initializer. |
| Forward first-fit allocate | `0x08879DB4..0x08879EC4` | `0x78A68..0x78B78` | `fef32a37a1c983f8e618b0bc198a7c9e174121e7279c44425306dbd091979b10` | Complete entry through all return branches. |
| Physical-node search | `0x08879EC4..0x08879F08` | `0x78B78..0x78BBC` | `3988acda81e1b4ece90f9643b3d4600acda74e8ca0b6d2329bb86ec2af8efa67` | Complete helper through return. |
| Reverse-end allocate | `0x08879F08..0x08879FF0` | `0x78BBC..0x78CA4` | `816c4832a907c45022855cea456ab5e4c00cbb8a32f8a8a6318ff84c4ea18d10` | Complete entry; calls the physical-node search helper. This is another allocation direction, not a pointer-preserving resize. |
| Free and coalesce | `0x08879FF0..0x0887A104` | `0x78CA4..0x78DB8` | `e0e39dc206acc247753d0b5312558efca7a7f495b92aa201eb30c7afbc989596` | Complete entry through all return branches. |
| Tested texture caller tail | `0x088B03B0..0x088B0434` | `0xAF064..0xAF0E8` | `53fbebd4e4702ab07b244cb99064a6dc36b79d553964729e9d6d0fe5a45e8d60` | Bounded tail after the virtual provider, through indexed child lookup and return; **not** the complete enclosing function. |
| Texture call fragment | `0x088B03C4..0x088B0400` | `0xAF078..0xAF0B4` | `e46e9cdeb05e7794230162b9adf60d6ac4a14e830d260904f360e6b4ca78b5e7` | **Local fragment only**, from child-count read through the builder call's delay slot; this is not the complete enclosing function. |

The caller fragment reads `u32(child+8)`, computes `36 * count` with 32-bit
shifts/addition, loads a manager pointer from guest global `0x09FBE75C`, and
calls `0x08879DB4` with `(manager, request_bytes, alignment=16)`. It passes
the returned pointer to the certified builder at `0x0889E5C0` with state
`r30+5104`, the same child pointer, and zero first-slot, first-record, and
count-override registers. The builder therefore uses the child's header count.
The fragment contains no successful-allocation check between the two calls.
The global is in the ELF load segment's memory-only region, not a file-backed
initialized pointer. The enclosing caller and source-resource provenance need
their own full-span evidence; this fragment does not certify either.

## Manager and node layout

The initializer receives `r4=manager`, `r5=heap_base`, `r6=heap_size_bytes`.
It writes manager `+0=heap_size_bytes`, `+4=heap_base`, then tail-branches to
reset. Reset places a 32-byte sentinel node at `heap_base`, zeros node
`+0/+4/+8/+12`, sets node `+16=2`, `+20=(heap_size_bytes-32)>>4`, `+24=0`,
and sets manager `+16=heap_base`. `manager+8` acts as the preceding-free-link
sentinel because its `+8` field is manager `+16`. Reset does not clear the heap
payload or older returned pointers. The code has no size, mapping, or overflow
validation; the arithmetic here is unsigned 32-bit.

| Offset | Observed node use |
| --- | --- |
| `+0`, `+4` | Physical links toward higher and lower node addresses. |
| `+8`, `+12` | Next and previous free-list links; the previous link may be `manager+8`. |
| `+16` | Node's reserved span in 16-byte units (`2` for the initial sentinel). |
| `+20` | Available free span in 16-byte units following this node. A positive value makes the node eligible for allocation. |
| `+24` | Alignment adjustment used when computing the following node's start. |
| `+28` | No read/write in the listed allocator entries. |

These offsets describe the original's linked structure, not a safe public
allocation header format. The original does not validate link integrity or
ownership tokens before dereferencing a returned pointer's preceding header.

## Forward allocation and usable extent

`0x08879DB4` receives `r4=manager`, `r5=request_bytes`, `r6=alignment`.
Zero `request_bytes` returns zero. Otherwise it computes
`Q = (request_bytes + alignment + 47) >> 4` using wrapping 32-bit arithmetic,
then walks the free list from `manager+16` via node `+8`. The first node `L`
whose `L+20 >= Q` supplies the allocation; if none exists, it returns zero.
The alignment mask is `-alignment`, so the useful checked domain requires a
nonzero power-of-two alignment and nonoverflowing arithmetic. The observed
texture caller uses 16.

For a selected node, let `U=load32(L+16)` and `D=load32(L+24)`. Under those
checked assumptions, the original computes:

```text
frontier = L + 16*U - D
returned = align_up(frontier + 32, alignment)
new_node = returned - 32
new_adjustment = new_node - frontier
new_node.units = Q
new_node.free_units = L.free_units - Q
L.free_units = 0
```

It inserts `new_node` into the physical chain. When residual free units are
positive, it replaces `L` in the free list with `new_node`; when zero, it
removes that free-list entry. The logical upper frontier of this allocation is
`new_node + 16*Q - new_adjustment`, so the allocator's nominal byte extent
starting at `returned` is `16*Q - new_adjustment - 32`. This is the distance
to the earliest following node header under the checked list invariant. It is
not the request size, and its slack must not be silently promoted to the
builder's authorized command capacity.

For `alignment=16` and a nonoverflowing request of `36*N` positive bytes,
the nominal extent exceeds the request by 1 to 31 bytes, depending on the
alignment adjustment. It still holds exactly `N` complete 36-byte command
slots. A captured successful allocation request can therefore supply the
slot count if the allocation remains live; the header count alone cannot.

For example, with a 16-aligned base and a fresh 0x400-byte heap,
`allocate(36,16)` has `Q=6`, returns `base+64`, and leaves a nominal 64-byte
extent from that pointer. The caller requested 36 bytes: one 36-byte command
slot. A zero child count requests zero and yields a null output pointer; the
builder's no-command path can still write its state fields. A positive count
with allocation failure is not checked at this caller and must not be treated
as a successful bounded adapter input.

The reverse-end entry `0x08879F08` has the same `(manager, request, alignment)`
inputs and unit formula. It calls `0x08879EC4`, which follows physical `+0`
links to the highest node and then descends via `+4` until it finds enough
free units. It places a new allocation at the high end of that free span,
updates physical/free links, and returns its header `+32`. A zero request
returns zero after the entry's stack save/restore. This entry is relevant to
heap reuse globally, but the cited texture fragment calls the forward entry.
For this reverse entry, alignment is applied before adding a residual-free-
unit displacement in multiples of 16. Thus the final returned pointer can
miss a requested alignment greater than 16; bounded original execution
observed ten such residues. Its 16-byte alignment held in that tested domain.

## Free, reset, and lifetime

`0x08879FF0` receives `r4=manager`, `r5=returned_pointer`. Null returns
without heap writes. For a nonnull pointer it reads the node at `pointer-32`.
If that node's lower physical link is null, it returns without coalescing;
otherwise it adds that node's reserved and free units to the lower node's free
units, removes the freed node from the physical chain, and removes, replaces,
or inserts the resulting lower node in the free list according to the two
nodes' free-link state. Future allocations can return the same address or
reuse overlapping bytes. No allocation generation, size token, pointer
provenance, double-free guard, or error result is established by this code.

Calling reset/initializer again makes all prior pointers logically stale even
if their bytes remain mapped and unchanged. A free/coalesce or reuse event can
invalidate an allocation without changing a separate cached extent. Physical
RAM aliases can also denote the same returned bytes. Neither a live resource
header count nor RAM mapping proves that the command pointer is still owned.
No pointer-preserving resize entry was found in this local allocator cluster;
allocations from either direction and free/reuse must be tracked separately.
The direct free site and lifetime of the specific texture command pointer
from `0x088B03C4` have not yet been paired by this audit.

## Production authority obligations

A production caller of the existing bounded guest adapter must obtain the
source extent from the resource owner and the command extent from a live,
successful allocation authority. For this command allocation, that authority
must capture the manager identity, returned pointer, requested byte count,
alignment, and either the verified node/free-list state at allocation or an
independently validated allocation record. It must attach a generation that
changes on free, heap reset/reinitialization, and address reuse; canonicalize
cached/uncached RAM aliases; and reject a pointer whose manager, generation,
current state, or requested capacity no longer matches. It must exclude
concurrent allocator mutation throughout validation and adapter commit.

The builder's selected output slots must fit the **trusted requested capacity**
(or a separately certified larger capacity), with integer overflow and slot
arithmetic checked. The source owner must independently prove the child extent
for the whole metadata traversal. Unknown, stale, aliased-to-protected, or
mutating state is outside the adapter's certified domain and must retain the
original path under a controlled fallback policy. The allocation/free/reset
events need scoped observation so a later paired run can establish actual
reachability and misses; counts alone are not lifetime evidence. Baseline
remains sealed and the current user-delivered applications remain unchanged.

## Bounded original execution

The separate root-run original gate at
`out/testing/texture-allocation-original-gate.json` (SHA-256
`e8526e62fce5eaba11c33e70c2707c1812576a76fd1c5e8f6ef6eaa046a0409e`)
reports 152 original AOT/interpreter calls on each path with identical full
RAM, VRAM, and CPU contexts. It includes 68 allocations, 60 frees, 16
initializations/resets, six exact-address reuses, exhaustion in a 256-byte
arena, and mixed forward/reverse allocation and free sequences. Eight tested
caller tails use synthetic child metadata with counts 1, 3, 17, and 75 in
cached and uncached RAM mirrors; they reach allocation and builder execution.
The maximum interpreter call took 18,063 instruction slices. This evidence
confirms the exercised behavior, not untested malformed heaps or a live
resource provider.

The direct free/reset of the **specific** texture command pointer from the
tested caller was not traced. The gate did not execute the live virtual source
provider, test concurrent heap mutation, or establish a source-ID-to-live-child
map. No live allocation authority or production hook is installed. These
remain prerequisites before the adapter can receive trusted live extents.


A later root gate, `out/testing/texture-allocation-provenance-gate.json`,
extends this to **168** top-level original execution segments per AOT and
interpreter path: 68 explicit allocation calls, 60 frees, 24 initializations
or resets, and 16 caller segments. Eight caller segments also execute the
original virtual slot-7 provider via vtable `0x0896FBC8`; the other eight begin
after it. All RAM, VRAM and CPU state agree after each segment. Six exact
address reuses and ten reverse requested-alignment residues remain observed;
the largest call takes 18,076 interpreter slices. Nested callees are included
in segment execution, not counted again as top-level calls.

The owner, heap and source contents are constructed fixtures. This extends
original code-flow evidence, but does not establish actual object creation,
loader completion or command-pointer release pairing. The runner binds ELF,
source and binary hashes, rejects incomplete coverage or broader readiness
claims, strips diagnostic environment overrides, bounds the child process to
60 seconds and publishes new evidence without overwriting old reports.
Reproduce after building `mhp3rd_texture_allocation_oracle`:

```sh
python3 profiles/mhp3rd/tools/check_texture_allocation.py \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_texture_allocation_oracle \
  --output out/testing/texture-allocation-provenance-rerun.json
```

Four synthetic runner-validation tests and strict C++20 compiler warnings
passed. ASSET-012 remains in progress until the source owner's creation and
load/reuse chain provide a usable live authority; this allocator evidence
alone does not close that task.


Follow-up evidence resolves the specific allocation-pointer-to-lobby-constructor
identity and selected command-pointer free pairing; see
[owner construction](TEXTURE_OWNER_OVERLAY_AUDIT.md),
[slot lifecycle](TEXTURE_SLOT_LIFECYCLE.md) and
[bounded owner validation](TEXTURE_OWNER_VALIDATION.md). Earlier unresolved
creation/free statements above describe the prior audit stage. Asynchronous
source completion and decoded extent remain unresolved; ASSET-012 is still
in progress.
