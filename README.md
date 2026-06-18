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
| **M1f** | Regression + fragmentation + randomized stress suite | ✅ done (68 checks) |
| **M1g** | `rename` / move (in-place + across dirs, journalled) | ✅ done |
| **M1h** | `free` — space + fragmentation accounting | ✅ done |
| **M1i** | Superblock recovery from secondary copy | ✅ done |
| **M1j** | Lazy AG init + global free counter (16 EB-class format) | ✅ done |
| **M1k** | Multi-cluster (extent-backed) directories | ✅ done (design/11) |
| **M2** | Journaling hooks spec + working `rewind` reference | ✅ done |
| **M3** | 256-drive support — gap analysis & change design | ✅ done |
| **M3a** | Dynamic drive/disc record tables (ARM) | ✅ build- **and runtime**-verified on RPCEmu (mount + file copy on a real disc) |
| **M3b.0** | Drive Record → 40 bytes (room for full-width index) | ✅ build+runtime-verified |
| **M3b.1** | Disc-record index → `DrvsDiscNum` (off the overloaded byte) | ✅ Phase A+B build+runtime-verified at `MaxDrives=8` |
| **M3c / >8** | `DiscsDrv` sentinel + loops + DiscOp64 drive routing + `MaxDrives` bump | 📋 fully specced (design/12) — atomic ~30-site sweep, needs a >8 test harness |
| M4 | G-format read support in FileCore (ARM) | planned |
| M5 | G-format write / allocation (ARM) | planned |
| M6 | Format/layout SWIs + ADFS/SCSIFS integration | planned |
| M7 | Journaling wired to G-format metadata path (ARM) | planned |

The host-side reference (`tools/gfcref`) already formats G-format images, stores and lists
files, integrity-checks the map against object extents, and journals/rewinds changes — all
verified off-target.

**ARM build pipeline is live, and M3a is runtime-verified.** The real FileCore module builds from
source in RPCEmu using the Acorn DDE (RISC OS 5 / IOMD, APCS-32) — to `rm.IOMD.FileCore` and a
softloadable `rm.IOMD.FileCoreSA`. **M3a** (dynamic drive/disc record tables: pointerised access,
then an `OS_Module`-claimed RMA block freed on Die — [`patches/m3a.diff`](patches/m3a.diff))
assembles + links with 0 errors **and runs**: softloaded over the ROM FileCore in a live RISC OS,
a 1600K ADFS floppy **mounts and copies files**. (Runtime testing earned its keep here: assemble +
ADFS-init looked green, but a real mount exposed a register-corruption bug — the `OS_Module` claim
trashed `R7`/`R8`, the floppy/winnie drive-existence bounds the init loop relies on, so every drive
was marked absent and mounts failed `BadDrive`. Fixed by preserving them across the SWI.) **M3b.0** (Drive Record grown to 40 bytes for a future full-width disc-record index, `DrvRecPtr`
×40) is build+runtime-verified — WIP diff [`patches/m3b-progress.diff`](patches/m3b-progress.diff).
**M3b.1 / M3c are deferred** as one coordinated effort: the index *storage* widening buys nothing
until the index *source* (the 3-bit field in disc addresses) is widened too, and migrating the
overloaded `DrvsDisc` byte in isolation proved fragile (a first attempt regressed at runtime, since
`DrvsDisc & 7` already serves as the number/0 the reads expect, and a separate byte must be kept in
lockstep at every write site). See [design/07 §6](design/07-M3bc-DriveNumberWidening.md) for the
full lesson and the holistic plan. The stable, runtime-verified foundation (M3a + M3b.0) stands.

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
directory tree whose **directories themselves grow across clusters** (extent-backed, like files —
design/11), journalled mutations with `rewind`, and superblock recovery from the secondary
copy. `check` verifies the decisive invariant disc-wide — each AG's allocated map == reserved ∪
every object's clusters incl. directory data clusters (no leaks/overlaps) — and `rewind` restores a
byte-identical earlier state (SHA-256), including directory-growth transactions. Reproducible
suite: `pwsh tools/gfcref/tests/regress.ps1` (68 checks). Build: `make` (gcc/MinGW).

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
- [`design/10-LazyAG-v1.md`](design/10-LazyAG-v1.md) — lazy AG init + global free counter (16 EB-class).
- [`design/11-MultiClusterDirs-v1.md`](design/11-MultiClusterDirs-v1.md) — multi-cluster (extent-backed) directories.
- [`design/12-MaxDrives256-Sweep.md`](design/12-MaxDrives256-Sweep.md) — complete `MaxDrives>8` change sweep (sentinel, loops, M3c, bump).

## Related bounty — #10 Partitioning (MBR + GPT)

[ROOL bounty #10](https://www.riscosopen.org/bounty/polls/10) is a complementary follow-on:
support for **MBR** and **GPT** partition tables, a rewritten **HForm** (Toolbox front end + a
scriptable interface for 3rd-party filing systems) that lays down / respects partitions before the
FileCore area, and adapting the desktop filers (one icon per physical drive today) and the `!Boot`
search to select a partition. #10 explicitly does **not** extend FileCore (it keeps the 2²⁹-sector /
2¹²-byte-sector / 20-partitions / 23-logical-drive limits) — it defers "huge drives" to a future
bounty, which is essentially this work (#40). So they dovetail: **#10 = the partition-table layer
around FileCore; #40 = the large-disc FileCore (256 drives × 16 EB, DiscOp64) for the huge drives
GPT enables.**

## License

FileCore upstream is Apache-2.0. Design documents here are released under the same terms unless noted.
