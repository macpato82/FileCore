# FileCore Large-Disc Format

Design and reference work for FileCore
adding a new native disc format to RISC OS's filing systems for large discs.

## Goal

A new FileCore disc format that:

- supports **up to 256 drives** per filing system (ADFS / SCSIFS / …),
- supports **up to 16 EB (2⁶⁴ bytes) per drive**,
- preserves RISC OS semantics — load/exec addresses, attributes, and **file-type** information,
- adds **journaling hooks** so 3rd-party modules can record disc changes and, in principle, rewind disc state,
- ships with a **recovery story** agreed with disc-tool vendors,
- extends FileCore internally per **DiscOp64** (64-bit sector addressing, drive number as its own byte).

Upstream source analysed: [`RiscOS/Sources/FileSys/FileCore`](https://gitlab.riscosopen.org/RiscOS/Sources/FileSys/FileCore) (v3.78).

## Why a new format

E+/F use a single **flat zone-bitmap** of the whole disc. That structure caps out around **8 TB** at usable
granularity — reaching 16 EB would require ~8 GB per map bit (≈176 GB minimum file size) and a ~256 MB flat map.
It cannot scale. See the spec for the full arithmetic and the design decision.

**Recommended direction:** a custom **"G format"** that keeps FileCore's object-id model, directories, and
file-type semantics, but replaces the flat map with **Allocation Groups** (each AG reuses the existing per-zone
bitmap code), widens object ids to ≥40 bits, and uses 64-bit sector addressing throughout.

## Status

| Milestone | Description | State |
|---|---|---|
| **M0** | Design specification + format reference | ✅ done |
| **M1** | C reference formatter + checker | ✅ done |
| **M1b** | Object & directory model (`mkfile` / `ls`) | ✅ done |
| **M1c** | Multi-extent, cross-AG files + `read` | ✅ done |
| **M1d** | `delete` (cross-AG free + reuse, journalled) | ✅ done |
| **M1e** | Subdirectories, paths (`mkdir`), recursive `check` | ✅ done |
| **M1f** | Regression + fragmentation + randomized stress suite | ✅ done (46 checks) |
| **M1g** | `rename` / move (in-place + across dirs, journalled) | ✅ done |
| **M1h** | `free` — space + fragmentation accounting | ✅ done |
| **M1i** | Superblock recovery from secondary copy | ✅ done |
| **M2** | Journaling hooks spec + working `rewind` reference | ✅ done |
| **M3** | 256-drive support — gap analysis & change design | ✅ done |
| **M3a** | Dynamic drive/disc record tables — implementation patch | ✅ drafted (build-unverified) |
| **M3b/c** | Drive-number widening (record index + address routing) | ✅ drafted (build-unverified) |
| M4 | G-format read support in FileCore (ARM) | planned |
| M5 | G-format write / allocation (ARM) | planned |
| M6 | Format/layout SWIs + ADFS/SCSIFS integration | planned |
| M7 | Journaling wired to G-format metadata path (ARM) | planned |

The host-side reference (`tools/gfcref`) already formats G-format images, stores and lists
files, integrity-checks the map against object extents, and journals/rewinds changes — all
verified off-target before any ARM-assembler work begins.

## Reference tool

[`tools/gfcref`](tools/gfcref/README.md) — `gfctool`, a host-side formatter/checker:

```
gfctool format  <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR]
gfctool mkfile  <image> <path> <srcfile>    # journalled; multi-extent, cross-AG
gfctool read    <image> <path> <outfile>
gfctool delete  <image> <path>              # rmdir if empty
gfctool mkdir   <image> <path>
gfctool rename  <image> <oldpath> <newpath>
gfctool ls      <image> [path]              # paths use '/' (-> RISC OS '.')
gfctool journal <image>
gfctool rewind  <image> [--to TXN]
gfctool check   <image>
gfctool info    <image>
gfctool free    <image>
```

A working mini-filesystem: files fragment into extents across allocation groups, a full
directory tree, journalled mutations with `rewind`, and superblock recovery from the secondary
copy. `check` verifies the decisive invariant disc-wide — each AG's allocated map == reserved ∪
every object's clusters (no leaks/overlaps) — and `rewind` restores a byte-identical earlier state
(SHA-256). Reproducible suite: `pwsh tools/gfcref/tests/regress.ps1` (46 checks). Build: `make` (gcc/MinGW).

## Layout

```
design/         Design documents and format specifications
tools/gfcref/   Host-side reference formatter / checker / journaller
tools/armcheck/ Host-side verifier for the ARM patch address arithmetic
```

- [`design/01-NewDiscFormat-Spec.md`](design/01-NewDiscFormat-Spec.md) — design spec & decision.
- [`design/02-GFormat-OnDisc-v1.md`](design/02-GFormat-OnDisc-v1.md) — byte-exact on-disc layout.
- [`design/03-ObjectModel-v1.md`](design/03-ObjectModel-v1.md) — object & directory model.
- [`design/04-Journaling-v1.md`](design/04-Journaling-v1.md) — journaling hooks (FileCore API + reference).
- [`design/05-256Drives-v1.md`](design/05-256Drives-v1.md) — 256-drive gap analysis & change design.
- [`design/06-M3a-DynamicDriveTables.md`](design/06-M3a-DynamicDriveTables.md) — M3a implementation patch (ARM).
- [`design/07-M3bc-DriveNumberWidening.md`](design/07-M3bc-DriveNumberWidening.md) — M3b/M3c drive-number widening (ARM).
- [`design/08-MultiExtent-CrossAG-v1.md`](design/08-MultiExtent-CrossAG-v1.md) — multi-extent, cross-AG files + `read`.
- [`design/09-Directories-v1.md`](design/09-Directories-v1.md) — subdirectories, paths, recursive `check`.

## License

FileCore upstream is Apache-2.0. Design documents here are released under the same terms unless noted.
