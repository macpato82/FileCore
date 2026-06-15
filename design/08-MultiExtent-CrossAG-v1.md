# G Format — Multi-Extent, Cross-AG Files + read (reference v1.1)

Extends [03-ObjectModel-v1.md](03-ObjectModel-v1.md). v1 stored every object as a single
contiguous cluster run inside AG 0. This stage proves the format's scaling mechanism: a file's
data may be **fragmented into multiple extents** and those extents may live in **different
allocation groups** — the basis for scaling past one AG toward 16 EB. Adds a `read` command so
file contents can be verified end-to-end.

## Object layout change (files)

A file is now stored as **a header cluster + N data clusters**:

- **Header cluster** (pointed to by the directory entry): the 64-byte object record followed by an
  **extent table**.
- **Data clusters**: the file bytes, located by the extent table; may be fragmented and cross-AG.

Directories are unchanged (single cluster, `ExtentCount = 0`, entries inline after the record).

### Object record (revised)
| Off | Sz | Field |
|----|----|-------|
| 0 | 4 | `OBJ_MAGIC` |
| 4 | 8 | ObjId |
| 12 | 1 | Type (1 file / 2 dir) |
| 13 | 1 | Attrs |
| 14 | 2 | **ExtentCount** (data extents; 0 for dirs) |
| 16 | 4 | Load |
| 20 | 4 | Exec |
| 24 | 8 | Length (bytes) |
| 32 | 8 | StartSector (this header cluster's own sector — recovery) |
| 40 | 8 | DataClusters (total data clusters = Σ extent counts) |
| 48 | 10 | Name |
| 60 | 4 | HdrCheck (sum over [0,60)) |
| **64** | ExtentCount × 16 | **extent table** (files) — or `EntryCount`+entries (dirs) |

### Extent (16 bytes)
| Off | Sz | Field |
|----|----|-------|
| 0 | 8 | StartSector (absolute) |
| 8 | 8 | ClusterCount |

Max extents = `(cluster − 64) / 16` (252 for a 4 KB cluster); exceeding it is an error in v1.

## Allocation

`mkfile` now:
1. Pre-checks total free clusters across **all** AGs ≥ `1 (header) + ceil(size/cluster)`.
2. Allocates the header cluster, then the data clusters, via a first-fit sweep across AGs that
   emits one extent per contiguous run — naturally producing fragmented / cross-AG layouts.
3. Updates **every touched AG**'s map (zone + cross checks) and header (`ClustersFree`,
   `LargestFreeRun`). All writes are journalled (one transaction), so `rewind` still works.

## read

`gfctool read <image> <name> <outfile>` — directory entry → header cluster → object record →
extent table → reads the data clusters in order and writes `Length` bytes to `outfile`.

## check (generalised)

For every AG *i*, expected map = structural-reserved (+ root in AG 0). Then walk the root: for each
object validate its record (`OBJ_MAGIC`, `HdrCheck`, `ObjId`), and mark its **header cluster** and
every **extent cluster** into whichever AG they fall in. Compare each AG's expected map to the
on-disc map and verify `ClustersFree`. This proves: no leaked/overlapping/double-allocated cluster
across the whole disc, including cross-AG objects.

## Test plan (runnable)
Format a disc with small AGs so a file must span them; write a file larger than one AG; `read` it
back and confirm byte-identical; `check` passes; corrupt an extent cluster bit → `check` fails.
