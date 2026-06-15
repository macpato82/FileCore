# G Format — Subdirectories & Paths (reference v1.2)

Extends the object model ([03](03-ObjectModel-v1.md), [08](08-MultiExtent-CrossAG-v1.md)) from a
single root directory to a full **directory tree**, giving the format the hierarchical, RISC-OS-style
namespace the bounty calls for.

## Model

A directory is an object exactly like the root: a single cluster holding the 64-byte object record
(`Type = dir`, `ExtentCount = 0`) followed by `EntryCount` and the entry array. A directory entry
whose `Type = dir` has `StartSector` pointing at the child directory's cluster. Files live in any
directory; their entry points at the file's header cluster (which carries the extent table, §08).

```
root ── docs ── sub ── f.bin (header cluster -> data extents, possibly cross-AG)
     └─ pics
```

> v1.2 keeps directories **single-cluster** (~100 entries at 4 KB; "directory full" on overflow).
> Multi-cluster directories are a later step. Sub-directory clusters are ordinary allocated
> clusters (not structural-reserved), so they are tracked by the allocator and the checker.

## Paths

Tool paths use `/` as the separator (mapping to RISC OS `.`). Components are resolved from the
root: `resolve_parent` descends all but the last component (used by `mkfile`/`mkdir`/`read`/
`delete`); `resolve_dir` descends every component (used by `ls [path]`).

## Commands

- `mkdir <image> <path>` — allocate a cluster, write an empty dir object, link it into the parent
  (journalled).
- `mkfile`/`read`/`delete` accept a path; they operate in the resolved parent directory.
- `delete` of a directory is refused unless it is empty (rmdir semantics).
- `ls <image> [path]` lists any directory.

## check (recursive)

`check` now walks the **whole tree** from the root (`collect_tree`, depth-guarded): it validates
each directory and file object (magic, header checksum), and collects every owned cluster —
sub-directory clusters, file header clusters, and all file extents — into a disc-wide set. The
per-AG comparison then proves each AG's allocation map equals the union of reserved clusters and
that set: no leaked, overlapping, or doubly-allocated cluster anywhere in the tree.

## Verified
A tree `docs/sub/` + `pics/` with a 150 KB file at `docs/sub/f.bin` (3 extents, cross-AG): path
listing at every level, byte-identical `read` of the nested file, recursive `check` passes,
non-empty `delete` refused, and full teardown (file → empty dirs) leaves a clean, consistent disc.
