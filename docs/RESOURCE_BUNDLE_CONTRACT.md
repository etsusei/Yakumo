# Indexed resource bundle contract (ASSET-001)

This note records the byte boundary that can be reused while replacing PSP-dependent resource handling. It is limited to the supported `NPJB-40001` inputs and static analysis. It does not assign a model, animation, texture, or other semantic type to every indexed child, and no game was started for this audit.

## Source identity and coordinate spaces

- The read-only executable was `profiles/mhp3rd/game/EBOOT.ELF`, SHA-256 `55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`. It is a little-endian MIPS ELF32 executable. The relevant `LOAD` segment maps file offset `0x2CB4` to guest address `0x08804000`; all code addresses below are guest virtual addresses in that segment, and the file offset of a code address is `0x2CB4 + (address - 0x08804000)`.
- The sample parent files are the already extracted, deobfuscated DATA.BIN entries in `profiles/mhp3rd/analysis/resources/raw-entries/`. Child offsets in this note are **relative to the start of a decoded parent entry**. They are not offsets into the ISO or obfuscated DATA.BIN. The parent entry itself retains its source span and hash in `manifest.json`.
- The generated C++ corpus was used only to cross-check static control flow. No generated code or original resource bytes are part of this document.
- Hashes, sampled spans, and call-site observations are also recorded in the ignored local evidence file `out/testing/resource-bundle-consumer.json`.

## Observed consumer behavior

The stripped ELF supplies no original function names. The following addresses identify the exact original-code spans, including each return delay slot. Full-span SHA-256 values permit a later bounded oracle to reject a different executable before running.

| Guest span, end exclusive | File span | SHA-256 | Direct observation |
| --- | --- | --- | --- |
| `0x088661BC..0x088661C8` (12 bytes) | `0x64E70..0x64E7C` | `4957f5372e34b70d76a56a232f14cdfbde36664457628f8b757aac861b817110` | Loads the 32-bit word at `a0 + 0` into `v0`. |
| `0x088661C8..0x08866234` (108 bytes) | `0x64E7C..0x64EE8` | `eb001e25ea896978ea3ce1a330e39005bc9db9e4d83ac44781bd86cf755957a8` | With parent pointer `a0` and index `a1`, compares the index with the first word using signed `slt`; for an in-range index, loads the word at `parent + 4 + 8 × index`. A zero word returns null; otherwise `v0` is `parent + word`. An out-of-range index returns null. |
| `0x08866234..0x0886628C` (88 bytes) | `0x64EE8..0x64F40` | `92fb871ca96bc51b757fbd85e212a212c0306516df2b481de7e6f2419f05306a` | With the same arguments and signed comparison, returns the word at `parent + 8 + 8 × index` for an in-range index, or `0xFFFFFFFF` for an out-of-range index. It does not test the offset word. |

The last two routines calculate `8 × index` from the parent base, so the first pair starts at offsets `+4` and `+8`, followed by records at an eight-byte stride. The pointer routine adds the first record word to the original parent base. This establishes the coordinate space and the first word's use as a record count for these consumers. The adjacent length routine returns the second record word unmodified. A scan of direct `jal` instructions in the main ELF found calls to the pointer routine but no direct call to the length routine; an indirect call or unused routine remains possible. Thus the byte field and accessor behavior are observed, while downstream use of advertised lengths is unknown.

These original routines trust their caller's buffer. They do not receive or check the parent byte length, detect arithmetic overflow, validate a signature, or check child overlap. Their signed comparison does not reject a negative index when the count is positive; such an index can address bytes before the table. A safe native reader should explicitly require a nonnegative index and bound the count, table, offset, and offset-plus-length within the parent. It should retain an in-range zero-offset record as an absent child slot, including its advertised length, rather than conflating it with an out-of-range index. Do not require zero padding, monotonic offsets, nonoverlap, alignment, or a zero-filled parent tail solely on the basis of these routines.

The pointer computation also permits aliasing: two nonzero records with the same offset return the same address, regardless of their advertised lengths. No parent-level magic is checked. The `pmo\0` and `.TMH` markers below are child-content observations and are not signatures required by this index accessor.

## Observed sample spans

Both parent entries have a count word of 3 and the three record pairs below. `pmo\0` and `.TMH` are observed four-byte markers at the indicated child starts; a marker alone does not establish the child's semantic type. The fields at `+4/+8` and all span calculations were read from immutable decoded entries, without exporting their bytes.

| Parent ID and SHA-256 | Child | Parent-relative offset | Advertised length | End exclusive | First four bytes |
| --- | ---: | ---: | ---: | ---: | --- |
| `00131` · `38cb2dc27506400495a3296a838313b87467b73f15268025de521a59db4e3b5a` | 0 | `0x20` | `0x5ABC0` | `0x5ABE0` | `pmo\0` |
| | 1 | `0x5ABE0` | `0x7A8` | `0x5B388` | `00 00 00 80` |
| | 2 | `0x5B390` | `0x309E0` | `0x8BD70` | `.TMH` |
| `01489` · `3f06d53ef775166b06a1bd98a8a03a0b79c5d895eb4ad652fc9ca3656a781885` | 0 | `0x20` | `0x33E0` | `0x3400` | `pmo\0` |
| | 1 | `0x3400` | `0x130` | `0x3530` | `00 00 00 80` |
| | 2 | `0x3530` | `0x2240` | `0x5770` | `.TMH` |

Parent `00131` is `0x8C000` bytes and parent `01489` is `0x5800` bytes. Every listed child is within its parent. In both, the 32-byte first payload offset leaves four zero bytes after the 28-byte table. The tails after the final advertised child are 656 and 144 bytes respectively and are mostly or entirely nonzero. Neither trailing region is an indexed child, and neither should be discarded from the raw parent. The first child of `01489` also contains the observed byte sequence `pmo\0` followed by `102\0`; this is a marker and version-like string observation only.

## Caller and lifetime evidence

At `0x088D150C..0x088D1670` (356 bytes including the return delay slot; file `0xD01C0..0xD0324`; SHA-256 `37c3e13b3655ad2451dd52b8b4e12f2151c21283cd447697b2d404a560a9d81b`), one caller holds its `a1` parent pointer in `s3`. It requests child indices 0, 3, 2, and conditionally 1 through `0x088661C8`. It reads halfwords at `+0x1C/+0x1E` of child 0, and passes the child pointers to downstream routines. This proves that callers use returned pointers as direct views into the parent, with optional children handled by null checks. It does **not** establish that this caller processed sample entry `00131` or `01489`; the source-ID-to-caller edge has not been observed.

The downstream code retains some of those pointers: `0x0889E17C` stores the child-0 pointer at object `+4`; one path through `0x0889E61C` stores the child-3 pointer at object `+0`; and `0x088D8728/0x088D872C` store the child-1 and child-2 pointers at object `+0x15C/+0x160`. These are pointer stores, not copies of the child bytes. The parent buffer therefore must remain alive for the lifetime of any object that dereferences those retained pointers. A native interface should return child views that share ownership of their parent bytes. The precise owner, release point, and whether other callers copy children remain unknown. The first pointer store is in the complete `0x0889E144..0x0889E22C` span (SHA-256 `2da0fbab9a1c669233090eb432204f1d6f632c3b3a60c1c12ab395d46f240c16`); the other two pointer-store clusters are fingerprinted as bounded windows in the local evidence file.

## Scope of the contract

**Validated for the supported samples:** the record layout, relative pointer computation, count use, zero-offset sentinel, and advertised span bounds agree between the original code and entries `00131` and `01489`. A separate structural survey may identify more candidates; its byte-pattern matches do not by themselves prove that each candidate is consumed by these routines.

**Unknown:** which DATA.BIN IDs reach which original callers; the meaning of each child type and any transform inside it; intended behavior for malformed counts, negative indices, overlapping children, nonzero unindexed tails, or absent children with nonzero advertised lengths; and the owning object's complete lifetime. This ASSET-001 contract was established statically. ASSET-002 subsequently executed the bounded originals and compared their index results, full CPU state and index/stack bytes; see [INDEXED_RESOURCE_VIEWS.md](INDEXED_RESOURCE_VIEWS.md). Any native resource loader should preserve the whole decoded parent and source-ID mapping while those questions are investigated. Gameplay, rendering, audio, and animation acceptance remain with later user-led tests.
