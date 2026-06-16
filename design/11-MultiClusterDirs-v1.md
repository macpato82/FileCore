# Multi-Cluster Directories (G format) — v1

Status: **proposed** (host-reference prototype for review; format-affecting).

## 1. Problem

In v1 a directory is a *single* cluster: the object record (64 B) followed by an inline
`EntryCount` (u32) and an array of 40-byte entries. Capacity is therefore

```
max_entries = (cluster_bytes - OBJ_HDR_BYTES - 4) / DIRENT_BYTES
```

— about 24 entries at a 1 KB cluster, ~100 at 4 KB. That hard cap is the last structural
limitation in the reference format: a directory cannot grow past one cluster, and the root
directory in particular cannot be relocated to make room.

## 2. Change: directories become extent-backed, like files

A directory uses the **same multi-extent mechanism as a file** (see
[08-MultiExtent-CrossAG](08-MultiExtent-CrossAG-v1.md)):

- The directory's **header cluster** holds the object record + the **extent table**
  (16 B per extent at offset `OBJ_HDR_BYTES`), exactly as a file does. `OBJ_Type = DIR`,
  `OBJ_ExtentCount` = number of data extents, `OBJ_ClusterCount` = total data clusters.
- The directory's **entry data is a byte stream** laid out as

  ```
  [ EntryCount : u32 ] [ entry 0 : 40 B ] [ entry 1 ] ...
  ```

  stored in the data clusters referenced by the extent table — read and written with the
  identical extent-walk used for file data. `OBJ_Length` = `4 + EntryCount * DIRENT_BYTES`.
- An **empty directory** has `ExtentCount = 0`, `ClusterCount = 0`, `Length = 4`; readers
  treat "no extents" as `EntryCount = 0`. The first added entry allocates the first data
  cluster (lazy, mirroring lazy AG init).

The entry record layout (`DIRENT_BYTES = 40`, name/type/attrs/load/exec/length/start) is
**unchanged** — only its *storage* moves from the header cluster into the extent-backed
data stream.

### Root directory

The root's header cluster stays at the fixed sector recorded in `SB_Root` and **never moves**
(legacy tools and the superblock both point at it). Its entry data grows via data extents like
any other directory, so the root is no longer capped at one cluster either.

## 3. Ripples

- **`dir_find` / `dir_add` / `dir_remove` / `ls`**: read the whole entry stream via the extent
  walk into a buffer, operate, write back; growth allocates clusters (cross-AG, journalled) and
  extends the extent table; shrink-on-delete compacts in place (clusters are not freed eagerly in
  v1 — a later compaction pass can return empty trailing clusters).
- **`check` / `collect_tree`**: a directory's owned clusters = header cluster ∪ every data
  extent's clusters; these join the per-AG "allocated == reserved ∪ owned" invariant exactly as
  file clusters do. Recursion descends into sub-directory header clusters as before.
- **Space accounting** (`free`, `SB_TotalClusters`/`SB_FreeClusters`): dir data clusters are
  ordinary usable clusters — counted used when allocated, like file clusters. No new accounting.
- **Journaling**: directory growth/mutation journals before-images of the touched header cluster,
  data clusters, and AG maps — no new record types; `rewind` already restores arbitrary cluster
  writes.

## 4. Compatibility

This changes the on-disc directory representation, so it is a **v1-format change, not an add-on**:
images written by the single-cluster code and the multi-cluster code are not interchangeable. As
the format is not yet frozen/vendor-reviewed, the reference tool simply adopts the new layout for
all directories (including an empty root at format time). The format version (`SB_DiscVersion`,
`GFC_FMT_MINOR`) is bumped so a reader can distinguish.

## 5. Verification

Extend the regression suite with: a directory forced past one cluster (hundreds of entries),
cross-cluster `ls`/`find`/`delete`, a sub-directory whose own directory spans multiple
extents/AGs, `check` proving the dir's data clusters are in the map union, and `rewind` of a
directory-growth transaction restoring a byte-identical image.
