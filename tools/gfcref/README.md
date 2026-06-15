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
gfctool format <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR]
gfctool check  <image>
gfctool info   <image>
```
Sizes accept `K/M/G/T/E` suffixes (powers of 1024). Defaults: 256 MiB image, 4096-byte
sectors, 64 MiB allocation groups, cluster = sector size.

### Example
```
$ gfctool format disc.img --size 256M --name MyDisc
Formatted disc.img: 256.00 MiB, 4 AGs, sector 4096, cluster 4096, 1 map-zone(s)/AG
$ gfctool check disc.img
CHECK OK: 4 AGs, 65536 sectors, all structures consistent
```

## What `check` verifies

1. Superblock checksum; primary == secondary copy; GFC magic + version.
2. Geometry self-consistency; image length == `TotalSectors × sector_size`; `AGCount`.
3. Every AG header: magic, AG number, base/offsets, checksum.
4. Every AG map: each zone's `ZoneCheck`; `CrossCheck` EOR == `0xFF`; allocation bits
   match the expected reserved set; `ClustersFree`/`ClustersTotal` fields.
5. Root directory check byte.

`check` reports the first errors with AG/zone/cluster locations and exits non-zero on
failure (verified against deliberate byte corruption).

## Files
- `gfc.h` — on-disc offsets, magics, geometry struct, little-endian accessors.
- `gfc_check.c` — check-byte algorithms ported from the FileCore sources.
- `gfctool.c` — geometry, `format`, `check`, `info`.

## Scope / v1 limitations
- Root directory is a minimal contiguous placeholder (full BigDir population is M1b).
- Per-AG allocation uses a **cluster bitmap + extents** model (see design/02 §3 rationale),
  not the E+ fragment/free-chain encoding; the production FileCore port may adopt either.
- `check` iterates every AG, so checking a genuinely huge image is O(AGCount); `info`
  is O(1) and validates the geometry math for any size up to 16 EB.
