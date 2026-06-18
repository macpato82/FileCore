# EXT4FS — ext2/3/4 filing system for RISC OS: design / implementation plan

Status: **plan (for discussion)**. **Own track**, separate from FileCore #40 and partitioning #10,
co-located here while planning (likely its own repo once implementation starts). Nothing implemented.

Agreed scope (2026-06-18):
- **Read-only first**, add **write** as a later phase.
- Cover **ext2 / ext3 / ext4** (one reader handles most of all three).
- A **standalone FileSwitch filing-system module** (working name **`EXT4FS`**), mountable from a
  partition handed over by the #10 `PartitionManager` (or from a whole disc / image).

## 1. Why and how it fits

Third strand of the "modern storage" goal: **#40** native FileCore large-disc, **#10** MBR/GPT
partition tables, **EXT4FS** foreign-filesystem support. Interlock: a GPT/MBR drive's Linux
partition (`0x83` / Linux GPT type-GUID) is recognised by `PartitionManager` and mounted by
`EXT4FS`, which reads sectors via the same **block-device callback / offset-DiscOp64** seam.

## 2. The on-disc format to read

- **Superblock** (offset 1024): block size, blocks/inodes per group, inode size, feature flags —
  crucially the `INCOMPAT`/`RO_COMPAT` masks (`extents`, `64bit`, `flex_bg`, `huge_file`,
  `metadata_csum`, `inline_data`, `encrypt`…). Refuse / degrade clearly on features we don't support.
- **Block group descriptors** (32- or 64-byte with `64bit`): locate inode/block bitmaps + inode table.
- **Inodes**: mode (→ RISC OS attrs/type), size (32+32 high), timestamps, and the data map —
  - **ext2/3**: 12 direct + single/double/triple **indirect** block pointers.
  - **ext4**: **extent tree** (`i_block` holds an `ext4_extent_header` + extents/index nodes).
  One reader handles both via an "iterate the data blocks of an inode" abstraction.
- **Directories**: classic **linear** `ext4_dir_entry_2` chains; **htree** (hashed) dirs read fine as
  linear for enumeration (the index is an optimisation we can ignore for read). Handle `filetype` in
  dir entries, `.`/`..`, and `symlinks` (fast + slow).
- **Journal (jbd2)**: read-only doesn't *write* the journal, but a **dirty** journal means the
  on-disk metadata may be stale. Phase plan: detect a dirty journal and **replay it into an
  in-memory overlay** for a consistent view (or, minimally, warn / mount cautiously). Decide per
  phase (see §6).

A host-side reference reader (parse real `mke2fs`/`mkfs.ext4` images, like the #40 `gfcref` tool)
de-risks all of this off-target with tests.

## 3. RISC OS integration

- **FileSwitch filing system**: implement the `FSEntry_*` handlers (Open, GetBytes, PutBytes(later),
  Args, Func, Close, File, …) and register with an **allocated FS number**. Mount a partition/disc
  as a RISC OS filing system (`EXT4FS::Disc.$.…`). (Alternative considered and rejected for the
  whole-partition case: a FileSwitch **ImageFS** — better suited to file-as-disc than a mounted
  partition.)
- **Semantics mapping (the interesting gap)** — ext has Unix mode bits, mtime/ctime/atime, owners,
  and **no RISC OS filetype/load/exec**:
  - *Filetype*: infer from a configurable **extension→type map** (e.g. `.txt`→Text, `.c`→C…), with a
    default (Data / Text); optionally honour a RISC OS-style `,xxx` suffix or a `user.RISCOS`
    xattr if present. Surface load/exec as a date-stamp (filetype + mtime) per RISC OS convention.
  - *Attributes*: Unix `rwx` (owner) → RISC OS read/write (+ locked from immutable / read-only).
  - *Case & length*: ext is case-sensitive with long UTF-8 names; map to RISC OS rules (case-insensitive
    lookup, `/`↔`.`, truncation/escaping policy for names RISC OS can't represent).
- **Block I/O**: read 512-byte sectors via the #10 block-device callback (offset by partition start),
  caching the superblock, group descriptors and recently-used metadata blocks.

## 4. Filetype / attribute mapping policy
Decide and document: the extension→filetype table source (built-in + a user-editable mapping file),
the default type, whether to honour embedded RISC OS metadata, and how access/timestamps round-trip.
This is the main place RISC OS users will notice "foreignness", so get it configurable.

## 5. Read-path architecture
`block device → superblock → group desc → inode lookup → (extent tree | indirect blocks) →
data blocks`; `directory inode → entry iteration → child inode`. Path resolution walks dir inodes
from the root inode (#2). All read-only, no allocation, no journal writes.

## 6. Phasing
1. **P0** design → (FS interface is FileSwitch-defined; module *Commands/FS number = allocations).
2. **P1** host-side reference reader + tests (real ext2/ext3/ext4 images; extent + indirect + htree
   traversal; semantics mapping table).
3. **P2** ext2 read (indirect blocks, linear dirs).
4. **P3** ext3 read (== ext2 for reads; recognise/skip the journal; handle a *clean* journal).
5. **P4** ext4 read (extent trees, 64-bit, `flex_bg`, htree-as-linear).
6. **P5** `EXT4FS` FileSwitch module: mount a partition (via #10) and the `FSEntry_*` read path.
7. **P6** dirty-journal replay into an in-memory overlay (consistent RO view of an unclean fs).
8. **P7** (later) write support — allocation, dir insert, and jbd2 journalling for crash safety.

## 7. Out of scope / risks
- Out (at least initially): ext4 **encryption**, **inline_data**, `metadata_csum` *verification*
  (read can ignore; writing later must maintain), **bigalloc**, case-folding (`casefold`).
- Risk: ext4's feature matrix is large — gate on `INCOMPAT`/`RO_COMPAT` masks and fail clearly on
  unsupported features rather than mis-reading.
- Write (P7) is genuinely hard (allocation invariants + jbd2) — kept firmly after a solid read story.

## 8. Relationship to #10 / #40
Shares #10's block-device-callback / `PartitionManager` mounting seam and #40's **DiscOp64** for
sectors beyond 32 bits on large drives. Independent of FileCore's own format (it's a *different*
filing system), so it doesn't touch the #40 work — it rides the same plumbing.

## 9. Open questions
1. FS presentation name / FS-number allocation; module name (`EXT4FS`?).
2. Filetype-mapping policy details (§4) — built-in table + user override file?
3. Dirty-journal handling for RO: replay-in-memory (P6) vs. warn-and-mount — acceptable interim?
4. Own repository at implementation time (recommended, given it's a separate track)?
