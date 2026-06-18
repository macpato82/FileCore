# Bounty #10 — Partitioning (MBR + GPT): design / implementation plan

Status: **plan (for discussion)**. This is a *separate* ROOL bounty
([#10](https://www.riscosopen.org/bounty/polls/10)) from the FileCore large-disc work (#40), but
complementary and co-located here while planning. Nothing is implemented yet.

## 1. Goal & scope (from the bounty)

- Support **both** partition schemes: **MBR** (DOS-origin; common on embedded) and **GPT** (modern,
  for large drives).
- A way for the user to **select a partition** (desktop filers show one icon per physical drive
  today).
- Adapt the **boot** process (the `!Boot` search) to pick the right / a default partition.
- Rewrite **HForm** to lay down partitions (or respect an existing scheme) before writing the
  FileCore area — with a desktop **Toolbox** front end **and** a **scriptable** interface so 3rd-party
  filing systems can reuse it.
- **FileCore is NOT extended here.** Its limits are retained: 2²⁹ sectors, 2¹² bytes/sector,
  **20 partitions/drive**, **23 logical drives**. (The "huge drives" extension is #40.)

## 2. Background — the RISC OS disc stack

```
FileSwitch                 (top-level filing-system switch)
  └─ FileCore              (shared disc-FS core: E/F/G format, maps, dirs)
       └─ low-level FS      ADFS / SCSIFS / SDFS / RAMFS
            └─ hardware     IDE / SCSI / USB-MSC / SD / SATA
```

A low-level FS registers a "disc" with FileCore (`FileCore_Create`) and services sector I/O via the
**DiscOp** entry FileCore calls. **Today one physical drive == one FileCore disc** — there is no
partition layer; each FS open-codes whatever ad-hoc scheme it needs (e.g. the Pi's FAT-boot + RISC OS
arrangement).

## 3. Proposed architecture

Introduce **one shared module — a *PartitionManager*** — that parses, enumerates, validates and
writes MBR/GPT tables on an abstract block device. The bounty's "scriptable interface so 3rd parties
needn't replicate the functionality" maps directly onto a shared module with a SWI + `*Command` API.

```
                 ┌──────────────┐
   low-level FS ─┤              │
   HForm        ─┤ Partition-   │── reads/writes MBR & GPT
   filers       ─┤   Manager    │   via a caller-supplied
   3rd-party FS ─┤              │   block-device callback
                 └──────────────┘
```

**Device abstraction (key decision).** PartitionManager must read/write sectors without knowing the
hardware, so the caller passes a **block-device descriptor**: `{ sector size, total sectors (64-bit),
a sector read/write handler entry + its workspace, flags }`. PartitionManager calls that handler
(in SVC, à la FileCore's DiscOp) to touch sectors. This decouples it from ADFS/SCSIFS/SD/SATA and
lets HForm operate on a raw device the same way a FS does.

**Logical-drive mapping (low-level FS side).** On identifying a physical drive a FS calls
`Partition_Enumerate`, then for each FileCore-formatted partition presents a **logical drive** to
FileCore whose DiscOps are **offset by the partition's start LBA**. FileCore is unchanged; it just
sees more (smaller) discs. The bounty's **23 logical drives** and **20 partitions/drive** are the
caps here. Start LBAs / sizes are 64-bit (GPT), so the offsetting uses **DiscOp64** — the bridge to
the #40 work.

## 4. Proposed module interface (text design — for review)

Individual SWIs (PartitionManager has 64 available), `R0` = flags throughout, 64-bit LBAs in register
pairs, the buffer-overflow / size-query and opaque-enumeration conventions from the module-design
guidelines. SWI base, error base, module name = **to be allocated** (ROOL allocation service).

- **`Partition_Identify`** — `(device desc) → scheme` (None / MBR / GPT / protective-MBR+GPT) and a
  validity summary. Cheap probe.
- **`Partition_Enumerate`** — opaque context in `R4` (`0` start, returns next, `-1` at end). Each
  step fills a caller buffer with one **partition record**: index, scheme, type (MBR type byte *or*
  GPT type-GUID), **start LBA (64-bit)**, **size in sectors (64-bit)**, attribute flags
  (bootable / read-only / **is-FileCore** / is-FAT / …), and (GPT) the partition name + GUID.
- **`Partition_ReadInfo`** — details for one partition index (random access alternative to enumerate).
- **`Partition_Create`** — add a partition of a given type/size/position to the in-memory table
  (incremental build, per the "append/update" guideline). Returns the assigned index.
- **`Partition_Delete`** — remove a partition from the in-memory table.
- **`Partition_Write` / `_Commit`** — write the (validated) table back to the device (MBR or GPT,
  incl. GPT's protective MBR, primary + backup headers, and CRC32s). Atomic where possible.
- **`Partition_Format`** *(scheme on a blank device)* — lay down a fresh empty MBR or GPT table.
- **`Service_PartitionsChanged`** — broadcast so filers refresh when a table is rewritten.

Suggested extras worth considering:
- A **"find bootable / preferred FileCore partition"** SWI so the boot path and filers share one
  policy (avoids each reimplementing "which partition holds `!Boot`").
- **Type-GUID constants** for "RISC OS FileCore" (we'd register a GPT partition-type GUID) and
  recognition of common foreign types (FAT, Linux, EFI System) for display.
- A **read-only mode flag** so enumeration on an untrusted/foreign disc never writes.

## 5. Scheme specifics

- **MBR**: 512-byte sector 0; 4 primary entries (type, CHS ignored, **32-bit** LBA start+size →
  caps at 2 TiB); **extended/logical** partitions via an EBR chain. Recognise an existing RISC OS
  convention for the FileCore partition type byte (to be pinned down / allocated).
- **GPT**: protective MBR in LBA 0; GPT header at LBA 1 (signature `EFI PART`, **CRC32** of header
  and of the entry array), partition entry array (typically 128 × 128-byte entries), **backup**
  header + array at the end of the disc; 64-bit LBAs; per-partition **type GUID**, unique GUID,
  attributes, UTF-16LE name. Validate both copies; repair from backup like FileCore's secondary
  superblock recovery (mirrors #40's `try_superblock`).

## 6. HForm rewrite

- Refactor HForm into a **scriptable core** (drives PartitionManager + FileCore format) plus a thin
  **Toolbox front end**. CLI/scriptable parity so 3rd-party FS and automation can drive it.
- New capability: choose a scheme, lay down / edit partitions, then FileCore-format a chosen
  partition; or **respect an existing table** and only (re)format one partition's FileCore area.
- Safety: confirm-before-write, read-only enumerate first, never touch foreign partitions
  unintentionally.

## 7. Filer & boot

- **Filer**: present FileCore-formatted partitions as selectable drives/icons (e.g. `SCSI::4.2`
  style, or a partition picker). Refresh on `Service_PartitionsChanged`.
- **Boot**: extend the `!Boot` search to walk partitions of a drive and pick the FileCore one
  holding `!Boot` (or a configured default), falling back sensibly when none is natively formatted.
  Reuse the "preferred partition" SWI (§4) so boot and filer agree.

## 8. Relationship to #40

#10 is the **partition-table layer**; #40 is the **large-disc FileCore** for the huge drives GPT
addresses. The shared seam is **64-bit sector addressing (DiscOp64)** — partition offsets and the
backup-GPT location need it on big drives, and #40 already threads DiscOp64 through FileCore. #10
keeps FileCore's format unchanged; #40 is where that changes.

## 9. Phasing

1. **P0** PartitionManager interface design → PRM-in-XML; register allocations (module name, SWI/error
   base, GPT type-GUID, MBR type byte, `*Command`s).
2. **P1** Read path: `Identify` / `Enumerate` / `ReadInfo` for MBR then GPT, with a host-side
   reference + tests (same approach as the #40 `gfcref` tool — parse real MBR/GPT images off-target).
3. **P2** Write path: `Create` / `Delete` / `Write` / `Format` incl. GPT CRC + backup + protective MBR.
4. **P3** Low-level FS integration: partition→logical-drive mapping with offset DiscOp64 (prototype in
   one FS, e.g. SCSIFS/SDFS on the Titanium).
5. **P4** HForm scriptable core + Toolbox front end.
6. **P5** Filer partition selection + boot-process partition pick.

## 10. Open questions (need decisions before PRM-in-XML)

1. **Module vs. in-FS**: shared **PartitionManager** module (recommended, matches "scriptable, reusable"
   wording) vs. baking it into each low-level FS. → assume shared module.
2. **Device-access model**: caller-supplied sector callback (recommended) vs. PartitionManager calling
   FileCore/FS directly.
3. **Drive↔partition naming** the filers/boot should expose (`SCSI::4.2`? a picker? a mount step?).
4. **MBR FileCore type byte** and **GPT FileCore type-GUID** — reuse an existing convention or
   register new ones.
5. **256 vs 255 / limits** — #10 keeps FileCore's 23 logical drives & 20 partitions/drive; confirm those
   are the working caps for the mapping layer.
