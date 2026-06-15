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
gfctool mkfile  <image> <name> <srcfile>    # journalled
gfctool ls      <image>
gfctool journal <image>
gfctool rewind  <image> [--to TXN]
gfctool check   <image>
gfctool info    <image>
```

`check` verifies the decisive invariant — allocated map bits == structural-reserved ∪ every
object extent (no leaks/overlaps) — and `rewind` restores a byte-identical earlier disc state
(confirmed by SHA-256). Build with `make` (gcc/MinGW).

## Layout

```
design/         Design documents and format specifications
tools/gfcref/   Host-side reference formatter / checker / journaller
```

- [`design/01-NewDiscFormat-Spec.md`](design/01-NewDiscFormat-Spec.md) — design spec & decision.
- [`design/02-GFormat-OnDisc-v1.md`](design/02-GFormat-OnDisc-v1.md) — byte-exact on-disc layout.
- [`design/03-ObjectModel-v1.md`](design/03-ObjectModel-v1.md) — object & directory model.
- [`design/04-Journaling-v1.md`](design/04-Journaling-v1.md) — journaling hooks (FileCore API + reference).
- [`design/05-256Drives-v1.md`](design/05-256Drives-v1.md) — 256-drive gap analysis & change design.
- [`design/06-M3a-DynamicDriveTables.md`](design/06-M3a-DynamicDriveTables.md) — M3a implementation patch (ARM).
- [`design/07-M3bc-DriveNumberWidening.md`](design/07-M3bc-DriveNumberWidening.md) — M3b/M3c drive-number widening (ARM).

## License

FileCore upstream is Apache-2.0. Design documents here are released under the same terms unless noted.
