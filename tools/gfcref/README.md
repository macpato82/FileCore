# gfcref — FileCore "G" format reference tool (M1)

Host-side reference **formatter** and **checker** for the proposed FileCore large-disc
"G" format ([bounty #40](https://www.riscosopen.org/bounty/polls/40)). It produces and
validates disc images off-target so the on-disc layout can be proven and sample images
handed to disc-recovery vendors before any ARM-assembler work.

Implements the byte-exact layout in [`design/02-GFormat-OnDisc-v1.md`](../../design/02-GFormat-OnDisc-v1.md),
including the FileCore zone check-byte (`map_zone_valid_byte`, `Doc/EMaps`), the cross-check
EOR rule, and the directory check byte (`Doc/Dirs`).

## Build

```
make            # -> gfctool(.exe)
make test       # build + format/info/check a 256 MiB image
```
Or directly: `gcc -std=c99 -O2 -Wall -o gfctool gfctool.c gfc_check.c`
(Tested with gcc/MinGW; 64-bit file offsets via `_fseeki64` on Windows, `fseeko` elsewhere.)

## Usage

```
gfctool format  <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR]
gfctool mkfile  <image> <path> <srcfile>   # journalled; multi-extent, cross-AG
gfctool read    <image> <path> <outfile>   # extract a file's contents
gfctool delete  <image> <path>              # journalled; frees clusters; rmdir if empty
gfctool mkdir   <image> <path>              # create a subdirectory (journalled)
gfctool rename  <image> <oldpath> <newpath> # rename / move (journalled)
gfctool ls      <image> [path]              # paths use '/' (-> RISC OS '.')
gfctool journal <image>                     # list journal transactions
gfctool rewind  <image> [--to TXN]          # undo last txn (or back to TXN)
gfctool check   <image>
gfctool info    <image>
```
Sizes accept `K/M/G/T/E` suffixes (powers of 1024). Defaults: 256 MiB image, 4096-byte
sectors, 64 MiB allocation groups, cluster = sector size.

### Example
```
$ gfctool format disc.img --size 64M --name Demo
Formatted disc.img: 64.00 MiB, 1 AGs, sector 4096, cluster 4096, 1 map-zone(s)/AG
$ gfctool mkfile disc.img hello.txt hello.txt
added 'hello.txt' (47 bytes, 1 cluster) at sector 5
$ gfctool ls disc.img
$ (1 object)
  hello.txt     file          47 bytes  load=fffffd00 exec=00000000
$ gfctool check disc.img
CHECK OK: 1 AGs, 16384 sectors, all structures consistent
```

## What `check` verifies

1. Superblock checksum; primary == secondary copy; GFC magic + version.
2. Geometry self-consistency; image length == `TotalSectors × sector_size`; `AGCount`.
3. Root + every object record: `OBJ_MAGIC`, header checksum, `ObjId`, `StartSector`,
   cluster run within range.
4. Every AG header (magic/number/offsets/checksum) and map (each zone's `ZoneCheck`;
   `CrossCheck` EOR == `0xFF`).
5. **Map ↔ extents consistency:** AG 0's allocated bits == structural-reserved ∪ root ∪
   every object run (no leaked, overlapping or doubly-allocated clusters);
   `ClustersFree`/`ClustersTotal`.

`check` reports the first errors with AG/zone/cluster/object locations and exits non-zero
on failure (verified against deliberate corruption of map bits and object headers).

## Journaling & rewind (M2)

`mkfile` runs inside a **transaction** and captures a before-image of every sector it
overwrites into a sidecar journal `‹image›.gfcjrnl` — the host-side mirror of the FileCore
journaling hooks specified in [design/04](../../design/04-Journaling-v1.md). `journal` lists
the transactions; `rewind` replays before-images in reverse to undo whole transactions.

```
$ gfctool mkfile disc.img b.dat b.dat
$ gfctool rewind disc.img
rewound 1 transaction(s), restored 4 write record(s); journal now ends before txn 7
```
Verified: after `rewind` the image is **byte-identical** (SHA-256) to its pre-transaction
state and `check` still passes; repeated `rewind` fully unwinds to the formatted disc.

## Tests

`tests/regress.ps1` is a reproducible regression + stress suite (38 checks): CRUD round-trips,
cross-AG multi-extent files, delete/reuse, nested directories, journal/rewind, corruption
detection, intra-AG fragmentation (a file threaded through punched holes), and a 60-op randomized
create/delete stress run that re-verifies `check` after every step and reads back every survivor.
```
pwsh tools/gfcref/tests/regress.ps1     # exits non-zero on any failure
```

## Files
- `gfc.h` — on-disc offsets, magics, geometry struct, little-endian accessors.
- `gfc_check.c` — check-byte algorithms ported from the FileCore sources.
- `gfctool.c` — geometry, `format`, `mkfile`, `ls`, `journal`, `rewind`, `check`, `info`.

## Multi-extent & cross-AG (v1.1)

Files are stored as a **header cluster** (object record + extent table) plus data clusters that
may be **fragmented into multiple extents spanning different allocation groups** — the mechanism
that scales the format past one AG (see [design/08](../../design/08-MultiExtent-CrossAG-v1.md)).
`mkfile` allocates across all AGs (first-fit, one extent per run); `read` follows the extent table;
`check` proves the per-AG map equals the union of every object's clusters across the whole disc.

Verified: a 200 KB file on a disc of 128-cluster AGs lands in 4 extents across 4 AGs, `read`s back
byte-identical (SHA-256), `check` passes, and `rewind` of that cross-AG write restores the
formatted image byte-for-byte.

## Directories & paths (v1.2)

Full directory tree (`mkdir`, nested paths); see [design/09](../../design/09-Directories-v1.md).
Paths use `/` (maps to RISC OS `.`). `check` walks the whole tree and proves each AG's map equals
the union of reserved clusters and every object's clusters (sub-dirs, file headers, all extents) —
no leaks/overlaps disc-wide. `delete` of a directory is refused unless empty (rmdir).

## Scope / v1 limitations
- Directories are single-cluster (~100 entries at 4 KB); overflow reports "directory full".
- Allocation is first-fit (no best-fit / locality optimisation).
- Per-AG allocation uses a **cluster bitmap + extents** model (see design/02 §3 and
  design/03), not the E+ fragment/free-chain; the production port may adopt either.
- `check`/`mkfile` iterate AG 0 (and `check` every AG), so they are O(AG); `info` is O(1)
  and validates the geometry math for any size up to 16 EB.
