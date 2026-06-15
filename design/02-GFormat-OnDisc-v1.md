# FileCore "G" Format — On-Disc Layout v1 (frozen for the reference implementation)

This freezes the byte-exact layout that the host-side reference tool (`tools/gfcref`) formats
and checks. It is **v1 of the reference**, deliberately minimal but fully specified. Where it
refines [the design spec](01-NewDiscFormat-Spec.md), the rationale is called out.

All multi-byte fields are **little-endian** (ARM LE), matching FileCore.

---

## Frozen decisions (resolving spec §11)

| # | Question | v1 decision | Rationale |
|---|---|---|---|
| 1 | AGs vs extent B-tree | **Allocation Groups** | Reuses per-zone bitmap concept; lowest risk. |
| 2 | AG size | **Fixed `Log2AGSizeSectors`**, uniform; last AG may be short | Bases are computed, no AG table needed → scales to 16 EB with no metadata blowup. |
| 3 | Per-AG allocation encoding | **Zoned *cluster bitmap* (1 bit = 1 cluster, set ⇒ allocated)** with FileCore zone check-bytes; object ownership via **extent lists in object headers** | *Refines the spec:* instead of cloning E+'s fragment/free-chain + BigMap quirks, v1 uses a plain bitmap + explicit extents. Trivially verifiable (popcount = used), clean recovery, still a per-zone bitmap. The production FileCore port may instead reuse the E+ fragment encoding per-AG — interchangeable at the format boundary. **Flagged for review.** |
| 4 | Object id | `(AGNumber << ObjIdLocalBits) \| LocalId`, `ObjIdLocalBits = 15`, `ObjIdAGBits ≤ 40` | ≥ 40-bit AG space → 16 EB; 15-bit local keeps each AG a classic small map. |
| 5 | Journaling on-disc area | **None in v1** (feature-flag bit reserved); journal is client-owned | Keeps FileCore format-agnostic about journal storage (spec §7). |
| 6 | Sector sizes | 512 / 1024 / 2048 / 4096; **default 4096** | — |
| 7 | `bpmb` (cluster size) | `Log2bpmb ≥ Log2SectorSize` (cluster = whole sectors); **default = sector size** | Avoids sub-sector clusters; keeps cluster↔sector mapping integral. |
| 8 | Superblock copies | Primary @ sector 0, secondary @ sector 1 | Two self-checking copies for recovery (spec §8). |

> **Scaling insight (why AGs + idlen ≤ 15 works):** bounding each AG small keeps its map a classic
> single-sector-or-few-sector zone bitmap with ≤ 2¹⁵ objects, so we scale by **AG count** (≤ 2⁴⁰),
> not by widening any one map. AG count × AG size = up to 2⁴⁰ × 2²⁴ sectors × 4 KB ≈ 2⁶⁶ B > 16 EB.

---

## Disc geometry

```
sector_size   = 1 << Log2SectorSize           (512..4096)
cluster       = 1 << Log2bpmb  bytes          (>= sector_size)
secs/cluster  = 1 << (Log2bpmb - Log2SectorSize)
AG size       = 1 << Log2AGSizeSectors  sectors
AGCount       = ceil(TotalSectors / AGSizeSectors)     (last AG may be short)
AG i base     = i << Log2AGSizeSectors   (absolute sector)
```

AGs tile the whole disc from sector 0. The two superblocks physically occupy sectors 0 and 1,
which lie inside AG 0's range and are therefore marked **allocated** in AG 0's bitmap.

### Per-AG internal layout
```
AG i:  [ AG header : 1 sector ] [ AG map : MapZones sectors ] [ data clusters ... ]
       header sector = (i==0 ? 2 : base)      ; AG0 header sits after the 2 superblocks
       map  start    = header sector + 1
       data start    = map start + MapZones
```
`MapZones = ceil(ClustersPerAG / BitsPerZone)`, `BitsPerZone = sector_size*8 - 32`.

---

## Structures

### Superblock (sectors 0 and 1) — little-endian

Bytes 0..59 keep the **legacy FileCore disc-record shape** (offsets per `hdr/FileCore`) so legacy
tools can read `Log2SectorSize`/version and refuse a G disc cleanly. Bytes 64+ are the G extension.

| Off | Sz | Field | Notes |
|----|----|-------|-------|
| 0 | 1 | Log2SectorSize | |
| 1 | 1 | SecsPerTrk | 0 (N/A) |
| 2 | 1 | Heads | 0 |
| 3 | 1 | Density | 0 (hard disc) |
| 4 | 1 | IdLen | per-AG fragment-id width (≤ 15; informational in v1) |
| 5 | 1 | Log2bpmb | |
| 6 | 1 | Skew | 0 |
| 7 | 1 | BootOpt | 0 |
| 8 | 1 | LowSector | 0 |
| 9 | 1 | NZones (low) | low byte of MapZones (legacy shape) |
| 10 | 2 | ZoneSpare | 0 |
| 12 | 4 | Root (legacy) | 0 — real root is `RootObjId` below |
| 16 | 4 | DiscSize (low) | total size in **bytes**, low 32 |
| 20 | 2 | DiscId | |
| 22 | 10 | DiscName | space-padded |
| 32 | 4 | DiscType | `&FFD` (Data) |
| 36 | 4 | DiscSize2 | total size in **bytes**, high 32 |
| 40 | 1 | BigMap_ShareSize | 0 |
| 41 | 1 | BigMap_Flags | bit0 BigFlag = 1 |
| 42 | 1 | NZones2 | high byte of MapZones |
| 43 | 1 | reserved | 0 |
| 44 | 4 | DiscVersion | `GFC_DISC_VERSION` (distinct → legacy tools refuse) |
| 48 | 4 | RootDirSize | bytes |
| 52 | 8 | reserved | 0 |
| **64** | 4 | **GFC_SB_MAGIC** | `'G','F','C','1'` = 0x31434647 LE |
| 68 | 2 | FormatVersionMajor | 1 |
| 70 | 2 | FormatVersionMinor | 0 |
| 72 | 8 | TotalSectors | authoritative disc size, in sectors |
| 80 | 8 | AGCount | |
| 88 | 1 | Log2AGSizeSectors | |
| 89 | 1 | Log2SectorSize (echo) | |
| 90 | 2 | MapZones (per AG) | |
| 92 | 4 | FeatureFlags | bit0 journal-area, bit1 btree-freespace (both 0 in v1) |
| 96 | 1 | ObjIdAGBits | |
| 97 | 1 | ObjIdLocalBits | 15 |
| 98 | 2 | reserved | |
| 100 | 8 | RootObjId | `(0 << 15) | rootLocal` (root lives in AG 0) |
| 108 | 4 | DataStartInAG | sector offset of data within an AG (= 1 + MapZones, AG0 differs by SB offset) |
| 112 | 4 | ClustersPerAG | |
| 116 | 1 | Log2SecsPerCluster | |
| 117 | … | reserved (zeroed) | |
| sector_size-4 | 4 | **SBCheck** | checksum over bytes [0, sector_size-4) (see Algorithms) |

### AG header (1 sector) — little-endian

| Off | Sz | Field |
|----|----|-------|
| 0 | 4 | AGH_MAGIC `'G','F','A','G'` = 0x47414647 LE |
| 4 | 8 | AGNumber |
| 12 | 8 | AGBaseSector (absolute) |
| 20 | 8 | AGSizeSectors (this AG; last may be short) |
| 28 | 8 | HeaderSector (absolute) |
| 36 | 8 | MapStartSector (absolute) |
| 44 | 2 | MapZones |
| 46 | 2 | reserved |
| 48 | 8 | DataStartSector (absolute) |
| 56 | 8 | ClustersTotal (allocatable clusters in AG) |
| 64 | 8 | ClustersFree |
| 72 | 8 | LargestFreeRun (clusters) |
| 80 | … | reserved (zeroed) |
| sector_size-4 | 4 | AGHCheck (checksum over [0, sector_size-4)) |

### AG map zone (1 sector each) — little-endian

| Off | Sz | Field |
|----|----|-------|
| 0 | 1 | ZoneCheck (FileCore `map_zone_valid_byte`) |
| 1 | 1 | CrossCheck (EOR of all zones' CrossCheck == 0xFF) |
| 2 | 2 | reserved (0) |
| 4 | … | cluster bits, **LSB-first**; bit set ⇒ cluster allocated |

The first `ClustersPerAG` bits across the zones map AG clusters 0..N-1. Bits past the AG's real
cluster count (tail padding, and any in the final short AG) are set (allocated/reserved).

### Root directory (v1: minimal empty BigDir-style dir, 1 cluster in AG 0)

| Off | Sz | Field |
|----|----|-------|
| 0 | 1 | StartMasSeq |
| 1 | 4 | StartName `'H','u','g','o'` |
| 5 | … | (no entries) |
| tail | 1 | LastMark = 0 |
| | 3 | Parent (self / 0) |
| | 19 | Title (space-padded) |
| | 10 | Name (space-padded) |
| | 1 | EndMasSeq (== StartMasSeq) |
| | 4 | EndName `'H','u','g','o'` |
| | 1 | DirCheckByte (`Doc/Dirs` algorithm) |

> v1 root dir is intentionally a placeholder proving allocation + check-byte; full BigDir
> fidelity (variable size, entry packing) is M1b.

---

## Algorithms (ported from the FileCore sources)

- **ZoneCheck** — `map_zone_valid_byte` from `Doc/EMaps` (4-vector carry-chained word sum, folded
  to a byte; byte 0 of the zone excluded so it can hold the result).
- **CrossCheck** — EOR of every zone's CrossCheck byte must equal `0xFF` (`Doc/EMaps`). v1 sets
  zones 0..n-2 to 0 and zone n-1 so the total EOR is 0xFF.
- **DirCheckByte** — accumulation `EOR r0, r1, r0, ROR #13` over used dir bytes (`Doc/Dirs`).
- **SBCheck / AGHCheck** — new structures, so a dedicated 32-bit checksum: sum of all words in
  `[0, sector_size-4)` mod 2³², little-endian. (Documented here; not a FileCore algorithm.)

---

## What the checker verifies

1. Primary SB == secondary SB; `GFC_SB_MAGIC`, version, `SBCheck`.
2. Geometry self-consistency: `AGCount == ceil(TotalSectors/AGSize)`, sizes/offsets in range,
   image length == `TotalSectors * sector_size`.
3. Each AG header: `AGH_MAGIC`, `AGNumber == i`, base/size/offsets match computed geometry, `AGHCheck`.
4. Each AG map: every zone `ZoneCheck` valid; `CrossCheck` EOR == 0xFF; reserved structural
   clusters (SBs in AG0, header, map, root) marked allocated; tail padding allocated;
   `ClustersFree` in header == counted free bits.
5. Root dir `DirCheckByte` valid.
