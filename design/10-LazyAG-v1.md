# G Format — Lazy AG Initialisation + Global Free Counter (reference v1.3)

The whole point of bounty #40 is capacities (up to 16 EB) at which you **cannot** write metadata
for every allocation group up front. This stage makes `format` and the accounting independent of
disc size, so a 16 EB-class disc formats instantly and is fully usable.

## Lazy allocation groups

- `format` writes the two superblocks and **only AG 0** (which holds the root). AGs 1..N-1 are
  left uninitialised; the image is sparse/short. A `FEATURE_LAZY_AG` flag is set in the superblock
  (auto-enabled when `AGCount > 65536`, or forced with `--lazy`).
- An AG is "initialised" iff its header sector carries `AGH_MAGIC`. `ag_init_map` reads an AG's
  map, or, on first use, lazily writes its header + reserved map.
- `alloc_clusters` walks AGs only until the request is satisfied (normally AG 0), initialising an
  AG the moment it first allocates there. So work is proportional to data written, never to disc
  size. A file larger than AG 0 spills into AG 1, 2, … which are initialised on demand.

## Global free-space counter

Iterating every AG to total free space is impossible at 16 EB, so the superblock carries
`TotalClusters` (usable, object-holding clusters disc-wide, computed analytically — O(1)) and
`FreeClusters` (a running counter). `mkfile`/`mkdir` decrement it, `delete` increments it, all
inside the journalled transaction (so `rewind` restores it too). `free` reports it in O(1).

`check` verifies the counter is exactly consistent: `FreeClusters + objects + root == TotalClusters`,
and that `TotalClusters` equals the analytic usable total.

## check at scale

`check` no longer iterates every AG. It walks the directory tree, collects the set of AGs that
actually own object clusters, and validates **AG 0 plus those AGs** (each must be initialised) —
O(used AGs), not O(disc size). The map↔objects, zone-check and per-AG free invariants are unchanged
for the AGs it does validate.

## Verified
- `format --size 8E` (8 EiB): completes instantly, host image ~20 KB, reports 137,438,953,472 AGs /
  2.25 × 10¹⁵ sectors / ~2.25 × 10¹⁵ usable clusters; `check` passes (O(used AGs)); a file can be
  written and read back and `check` still passes; `free` is O(1).
- Lazy spill: a 100 KB file on a `--lazy` disc with 32 KB AGs is allocated across AGs 0..3
  (initialised on demand), reads back byte-identical, and `check` passes.

> A real >2 PB physical image isn't materialised (and would exceed host-FS limits); lazy mode proves
> the format and accounting scale, while `info`/`free`/`check` operate purely on the superblock and
> the (tiny) used metadata.
