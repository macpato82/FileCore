# FileCore "G" Format — Object & Directory Model v1

Extends [02-GFormat-OnDisc-v1.md](02-GFormat-OnDisc-v1.md) with the object layer the reference
tool uses to store real files and directories. All fields little-endian.

## Model

The per-AG map records only **free vs. allocated** clusters. **Ownership** lives in
self-describing **object records**, not in the map (this is the "bitmap + extents" decision,
design/02 §3). Each object begins with a 64-byte object record at the start of its first cluster;
file data / directory entries follow in the same contiguous cluster run.

> **v1 simplification:** every object is a single **contiguous** cluster run (one extent:
> `StartSector` + `ClusterCount`). Fragmented objects / multi-extent lists are a later milestone.
> All user objects are allocated in **AG 0** in v1.

## Object record (64 bytes, at object's first cluster)

| Off | Sz | Field |
|----|----|-------|
| 0 | 4 | `OBJ_MAGIC` `'G','F','O','B'` = 0x424F4647 LE |
| 4 | 8 | ObjId |
| 12 | 1 | Type (1 = file, 2 = dir) |
| 13 | 1 | Attrs (FileCore object attributes) |
| 14 | 2 | reserved |
| 16 | 4 | Load |
| 20 | 4 | Exec |
| 24 | 8 | Length (bytes; for a dir = used directory bytes) |
| 32 | 8 | StartSector (absolute, redundant — aids recovery) |
| 40 | 8 | ClusterCount |
| 48 | 10 | Name (space-padded) |
| 58 | 2 | reserved |
| 60 | 4 | HdrCheck (32-bit word sum over bytes [0,60)) |

File data begins at offset 64 of the first cluster. Usable file capacity of a run is
`ClusterCount × cluster − 64` bytes.

## Directory contents (follow the object record, Type = dir)

| Off (from 64) | Sz | Field |
|----|----|-------|
| 0 | 4 | EntryCount |
| 4 | EntryCount × 40 | entries |

### Directory entry (40 bytes)

| Off | Sz | Field |
|----|----|-------|
| 0 | 12 | Name (space-padded, control-char terminated) |
| 12 | 1 | Type |
| 13 | 1 | Attrs |
| 14 | 2 | reserved |
| 16 | 4 | Load |
| 20 | 4 | Exec |
| 24 | 8 | Length |
| 32 | 8 | StartSector (absolute, of the entry's object record) |

The root directory is the object at `RootObjId`, occupying AG 0's first data cluster
(reserved at format time). v1 root is a single cluster (~100 entries at 4 KB); overflow is
reported as "root full" pending multi-cluster directories.

## Checker additions

Walking from the root, `check` now also verifies:
- each object record's `OBJ_MAGIC`, `HdrCheck`, and `ObjId` vs. the referring entry;
- each object's cluster run lies within the disc and is fully marked allocated;
- **map consistency:** AG 0's allocated bits == structural-reserved ∪ root ∪ every object run —
  i.e. no leaked, overlapping, or doubly-allocated clusters; `ClustersFree` matches.
