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
gfctool mkfile  <image> <name> <srcfile>   # journalled mutation
gfctool ls      <image>
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

## Files
- `gfc.h` — on-disc offsets, magics, geometry struct, little-endian accessors.
- `gfc_check.c` — check-byte algorithms ported from the FileCore sources.
- `gfctool.c` — geometry, `format`, `mkfile`, `ls`, `journal`, `rewind`, `check`, `info`.

## Scope / v1 limitations
- Objects are **single contiguous cluster runs** (one extent); fragmented / multi-extent
  objects and sub-directory creation (`mkdir`) are a later milestone.
- All user objects are allocated in **AG 0**; cross-AG allocation is later.
- Root directory is a single cluster (~100 entries at 4 KB); overflow reports "root full".
- Per-AG allocation uses a **cluster bitmap + extents** model (see design/02 §3 and
  design/03), not the E+ fragment/free-chain; the production port may adopt either.
- `check`/`mkfile` iterate AG 0 (and `check` every AG), so they are O(AG); `info` is O(1)
  and validates the geometry math for any size up to 16 EB.
