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
| **M0** | Design specification + format reference | ✅ drafted |
| M1 | C reference formatter + checker + sample images | planned |
| M2 | Journaling hooks (register / service-call / txn) on existing E+ | planned |
| M3 | 256-drive internal support | planned |
| M4 | G-format read support in FileCore | planned |
| M5 | G-format write / allocation | planned |
| M6 | Format/layout SWIs + ADFS/SCSIFS integration | planned |
| M7 | Journaling wired to G-format metadata path | planned |

M1–M3 can proceed in parallel; each delivers standalone value.

## Layout

```
design/   Design documents and format specifications
```

- [`design/01-NewDiscFormat-Spec.md`](design/01-NewDiscFormat-Spec.md) — full design spec (current draft).

## License

FileCore upstream is Apache-2.0. Design documents here are released under the same terms unless noted.
