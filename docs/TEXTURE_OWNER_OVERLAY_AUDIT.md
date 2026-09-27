# Texture source owner: task-overlay constructor audit (ASSET-012)

This audit ties the candidate source-provider vtable `0x0896FBC8` to an actual
constructor in the original `lobby_task.ovl` and ties that constructor's `a0`
input to the pointer carried through the main-ELF placement call when that
overlay is resident. It does not
establish when resource bytes enter selector 7, a successful load/ready state,
or a complete release/reset pair. No game was run, and no production hook was
enabled. "Observed" below means original bytes and control/data flow;
"unknown" marks a missing ownership or lifetime edge.

## Original input and address coordinates

The supported main ELF is `profiles/mhp3rd/game/EBOOT.ELF`, SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Its first `LOAD` segment maps guest `0x08804000` to ELF file offset `0x2CB4`.
The raw overlay entries are indexed by
`profiles/mhp3rd/analysis/resources/manifest.json`:

| Raw entry | Header name / ID | Raw SHA-256 | Header load / code size |
| --- | --- | --- | --- |
| `raw-entries/00122.bin` | `lobby_task.ovl` / 162 | `c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca` | `0x0A05E600` / `0x118B24` |
| `raw-entries/00123.bin` | `game_task.ovl` / 164 | `495bf504928da2626c760ccda1ee2742b29393e8c0a2697b085d7bf6c2b0479f` | `0x0A05E600` / `0x11C218` |

Each raw file begins with its 64-byte `MWo3` header **at** the load address.
Thus guest address `A` in either task overlay maps to original raw-entry
offset `A - 0x0A05E600`; code starts at raw offset `0x40`. This matches the
repository's header parser and `wrap_overlay.py` mapping. Analysis-only ELF
wrappers and disassembly are under ignored `out/testing/texture-owner/`; the
raw entries were not modified. All spans below use exclusive ends and refer
to the original raw bytes, not the wrapper's ELF file offsets.
The entry-122 header-plus-code FNV-1a is `F6300296C8D954E5`, matching the
available original lobby recompiler library's corpus identity.

## Constructor and exact pointer chain

| Original span | Original file span | SHA-256 | Observed flow |
| --- | --- | --- | --- |
| Lobby guest `0x0A0E7460..0x0A0E74D0` (112 bytes) | Entry 122 raw `0x88E60..0x88ED0` | `b494a03c35826fdac7700740b5a643ef46b221e095cafbcd5e081e34876519bc` | Saves input `a0` as the object pointer, calls base constructor `0x088B0678`, writes vptr `0x0896FBC8` to object+0 at `0x0A0E7484`, zero-fills object+`0x1470` through object+`0x2F470`, sets object+`0x274` to 1 and object+`0x270` to `0xC4`, clears four nearby bytes, and returns 1. Includes the return delay slot. |
| Main guest `0x088BD0F4..0x088BD120` (44 bytes) | ELF `0xBBDA8..0xBBDD4` | `210afb312941ec43aaac9e6a42ea126576a0f7992ed7e73f502cf42c5958bc9a` | Computes a 12-byte record from a bounded index and writes the same `s2` allocation pointer at record+4, the caller-derived key `s1+0x30` at record+0, and size `0x2F470` at record+8, before construction. |
| Main guest `0x088BD120..0x088BD150` (48 bytes) | ELF `0xBBDD4..0xBBE04` | `57f5f390c0b0b3e4e284f38c5d6d0dab60b54da6055f1f80642b5ec9fcb80cbb` | Passes the earlier `s2` allocation pointer as `a1` to `0x088A0EF0`, moves that call's result to `a0`, and directly calls `0x0A0E7460` at `0x088BD13C`. |
| Main guest `0x088A0EF0..0x088A0EFC` (12 bytes) | ELF `0x9FBA4..0x9FBB0` | `2d8b4d334f33c20c893621a044c3227e9852be5a457e2fbcff3f716d4e9e8525` | Returns `a1` unchanged in `v0`; it is a placement-style identity helper, **not** an allocator. |

Immediately before that main span, the path sets `a1=0x2F470` and
`a2=0x10`, calls the reverse allocator `0x08879F08` at `0x088BD074`, and
saves its returned pointer in `s2` at `0x088BD07C`. The nonzero path clears
`0x2F470` bytes at that same pointer before passing `s2` through the
identity helper to the lobby constructor. With the lobby overlay resident,
the constructor then establishes the **exact vptr on the exact returned
allocation base**. This closes the
specific size-coincidence gap noted in
[TEXTURE_CALLER_SOURCE_AUDIT.md](TEXTURE_CALLER_SOURCE_AUDIT.md): the factory
does not merely allocate an equally sized but unlinked block. The main-ELF
allocator's internal semantics and the remaining lifetime conditions are
tracked separately.

The record base used in the preceding span is
`s0+0x00B43130+index*12`, with `s0` loaded through global
`0x09FBE8D8`. At `0x088BD0B0`, `a1` receives `s1+0x30`;
`0x088BD118` stores that value as the record key. This is another exact pointer reference to the allocation,
but the record's ownership meaning, invalidation, and relation to a loaded
resource remain unproved.

The lobby constructor calls `0x088EE468`, which jumps to the main-ELF byte
fill routine `0x0880C468`. Its arguments are destination
`object+0x1470`, value 0, and length `0x2E000`; the end is exactly
`object+0x2F470`. The selected provider's slot 7 range,
`[object+0x27C70, object+0x2D470)`, lies wholly inside this cleared region.
The constructor therefore creates an empty slot; its vptr and unconditional
return value 1 do **not** establish a loaded resource or readiness for the
texture builder. The base constructor temporarily writes a different vptr,
`0x0896FA78`, at `0x088B06D0` before the lobby constructor writes the final
one. A registry must account for this construction phase.

## Overlay identity and other construction lead

Entry 122 also has a byte-for-byte identical constructor body at lobby guest
`0x0A0E8E88..0x0A0E8EF8`, original raw `0x8A888..0x8A8F8`, with the same
112-byte SHA-256. A scan of direct `j`/`jal` targets in the executable code
of the main ELF and both task overlays found one direct call to
`0x0A0E7460` (main `0x088BD13C`) and none to `0x0A0E8E88`. This does not
rule out indirect entry or data-held function pointers; the second body's
factory and lifetime are unknown.

At the **same guest address** `0x0A0E7460`, entry 123 has different bytes:
its 112-byte raw span `0x88E60..0x88ED0` has SHA-256
`d5ccdde3edc69544120822056c1d7a58b4bbe9361f4154e989d8474f85bf9988`.
There, `0x0A0E745C` jumps to `0x0A0E72E4` and `0x0A0E7460` is its delay-slot
instruction, not the lobby constructor entry. A static scan found the exact
`lui 0x0897; addiu 0xFBC8; sw` vptr assignment twice in entry 122 (the two
bodies above) and zero times in entry 123. These are overlay-specific facts;
a shared guest address is insufficient authority for the class.

## Remaining authority edges

The constructor gives a concrete object base and a cleared fixed-capacity
slot. It does not show which source ID is copied into selector 7, the exact
write length, whether that write completed successfully, or when the object
becomes eligible for `0x088B0398`'s builder call. A ready bit must be tied to
a successful source load/copy for this same allocation generation, not to the
constructor return or to the vptr alone. Teardown, partial reload, reset,
free, in-place reconstruction, or address reuse must revoke it. The main-ELF
vtable has teardown-like entries and a separate slot-copy path, described in
the caller-source audit; this overlay audit does not prove their concrete
invocation or completion for a live object.

The lobby overlay has no direct call to selected teardown routines
`0x088BA6E4`, `0x088BA74C`, or `0x088BA67C`, nor to slot copy
`0x088A5324`. Indirect/vtable calls remain possible. It does call main
helpers `0x088BA038` and `0x088BA2D4` at `0x0A0E7204/0x0A0E7214` and
`0x0A0E7230/0x0A0E7240` with a pointer loaded from global `0x09FBE794`;
the relationship of that global to this allocation has not been proved.
No production registry or dispatch should infer source readiness from these
static constructor facts alone.
