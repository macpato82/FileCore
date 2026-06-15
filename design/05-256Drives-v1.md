# 256-Drive Support — Gap Analysis & Change Design (M3)

Bounty #40 requires **up to 256 drives per filing system**. The DiscOp64 transport already
carries an 8-bit drive number; this milestone is about FileCore's *internal* drive/disc tables
and the legacy 3-bit drive field. All findings below are against
`RiscOS/Sources/FileSys/FileCore` (v3.78), cited by file:line.

This is an analysis/design deliverable (no host-tool code): the work lands in the ARM module.

---

## 1. The hard limits today

| # | Limit | Value | Evidence |
|---|---|---|---|
| L1 | Drive-record array | **8** drives | `s/FileCore00:274` `DrvRecs # SzDrvRec * 8` |
| L2 | Disc-record array | **8** discs | `s/FileCore00:275` `DiscRecs # SzDiscRec * 8` |
| L3 | **Workspace ceiling** | **< &1000 (4 KB)** for *all* static workspace incl. the record arrays | `s/FileCore00:285` `ASSERT :INDEX:{VAR}<&1000` |
| L4 | Legacy disc address drive field | **3 bits** (drives 0–7) | `hdr/FileCore:138-139` `LegacyDiscAddress_DriveNumber_Mask * 2_111`, `_Shift * (32-3)` |
| L5 | Drive-record disc index (`DrvsDisc` bits 0-2) | **3 bits** (8 discs) | `s/Defns:154-159` |
| L6 | Error disc-number field | **6 bits** (0–63) | `s/Errors:48` `MaxDiscErr * 2_00111111`; used `s/FileCore30:885,906,959,1371` |
| L7 | `MapSizeEstimate` hardwired to drives 4–7 | 4 fixed discs | `s/Defns:94-101` (legacy 4-floppy/4-winnie model) |

### What is **already** wide enough (no change)
- **DiscOp64 address block** — `ExtendedDiscAddress_DriveNumber # 1` is a full byte (`hdr/FileCore:148`). The 64-bit transport is 256-drive-ready.
- **`FileCore_Create` R3 config** — `DriveConfig_FloppyCount` and `FixedDiscCount` are **8 bits each** (`s/Defns:82-85`), so the creation API can already *declare* up to 255+255 drives. The limit is purely the internal tables, not the entry point.
- **`Format_G * 4`** is already reserved (`s/Defns:118`) — the new format's letter is allocated.

---

## 2. The real blocker: L3, not L1/L2

Naively you'd just change `* 8` → `* 256` in `s/FileCore00:274-275`. That is impossible:

```
SzDiscRec = 64 bytes                  → DiscRecs at *256 = 16 KB
SzDrvRec  ≈ 30–40 bytes               → DrvRecs  at *256 ≈ 9 KB
```

Either alone blows the **`< &1000` (4 KB)** workspace ceiling (`s/FileCore00:285`), which also
has to hold buffers, scatter lists, the dir buffer, etc. So the record arrays **cannot** stay as
static workspace.

**Design decision:** move `DrvRecs` and `DiscRecs` out of the fixed workspace into a
**dynamically-allocated, drive-count-sized table** (RMA / dynamic area), pointed to from
workspace, indexed by drive number / disc-record number. Allocate lazily to the configured drive
count (from `FileCore_Create` R3) rather than always 256. This keeps the common case (a handful of
drives) cheap and the workspace under &1000.

This mirrors how the codebase already moved other large per-instance structures to dynamic areas
(`DynamicMaps` for `DrvsFsMap*` at `s/Defns:175-179`; `BigDir` dir buffer at `s/FileCore00:289-292`).

---

## 3. Change plan

### 3.1 Records → dynamic, index ≥ 8 bits
- Replace `DrvRecs`/`DiscRecs` static arrays with `DrvRecsPtr`/`DiscRecsPtr` + `DrvCount`/`DiscRecCount` in workspace; allocate the tables at `FileCore_Create` time from R3's counts.
- Every `base + n*SzDrvRec` / `base + n*SzDiscRec` index site must load the pointer first. (Audit: `CritDrvRec`/`CritDiscRec` `s/FileCore00:239-240`, and all `DrvRecs`/`DiscRecs` references.)
- Widen `DrvsDisc` disc-record index (L5) from bits 0-2 to a **full byte** — i.e. split the flags (Empty/Full/Unknown/Uncertain) and the disc-record number into separate bytes in the Drive Record, since 8 bits of index leaves no room for the flags in one byte (`s/Defns:154-159`). `SzDrvRec` grows by 1; fix all `DrvsDisc` mask/shift sites.

### 3.2 Internal addresses → carry drive via DiscOp64, not packed bits
- The 3-bit `LegacyDiscAddress_DriveNumber` (L4) caps in-address drives at 8. The new format must
  **stop packing the drive into the top 3 bits** of a 32-bit disc address and instead pass the
  drive in the DiscOp64 drive byte (already defined). Internally, object/disc addresses become
  `(drive, 64-bit sector)` pairs (matches `design/02`'s object model, which already stores absolute
  sectors and a separate AG/drive concept).
- `s/FileCore15` `GenIndDiscOp`/`RetryDriveOp` and the `ProcessDrive`/`ProcessWriteBehindDrive`
  fields (`s/Defns:571,577`) are the conversion choke points — route them through DiscOp64.

### 3.3 Error encoding (L6)
- The 6-bit `MaxDiscErr` disc-number field (`s/Errors:48`, packed in `s/FileCore30:885-1371`) tops
  out at 63. For 256 drives the disc number in error returns must widen, or the error format change
  to carry the drive separately. Lower-risk option: keep error text generation but source the drive
  number from the DiscOp64 path rather than the 6-bit packed field.

### 3.4 Drop the 4–7 hardwiring (L7)
- `MapSizeEstimate_Drive4..7` (`s/Defns:94-101`) assumes the classic 0–3 floppy / 4–7 winnie layout.
  Already noted "unused with big discs"; for G format make map-size estimation drive-count-agnostic
  (derive from the disc record, not a fixed per-drive R6 nibble).

### 3.5 Command / API surface
- `*` drive commands, `FileCore_MiscOp` (drive in R1), mount/dismount, and the FileSwitch drive↔name
  mapping must accept drive numbers 0–255. Range checks currently assuming 0–7 need widening
  (audit alongside the `DrvRecs` indexing sweep).

---

## 4. Compatibility & sequencing

- **Backwards compatible by construction:** existing media uses ≤ 8 drives; with the tables
  allocated to the configured count, a normal machine is unchanged. Old discs keep the legacy
  3-bit-in-address scheme; only G-format / DiscOp64 paths use the wide drive number.
- **Independent of the new map format:** L1–L7 are pure FileCore-internal plumbing. M3 can land
  before/parallel to M4–M7, and immediately benefits any filing system that registers many drives.

### Sub-milestones
| | Step | Risk |
|---|---|---|
| M3a | Move `DrvRecs`/`DiscRecs` to a dynamic table sized from `FileCore_Create` R3; pointerise all index sites | medium (wide but mechanical) |
| M3b | Widen `DrvsDisc` disc-record index to 8 bits (restructure Drive Record) | low |
| M3c | Route internal addresses through the DiscOp64 drive byte; retire 3-bit packing on new paths | medium |
| M3d | Widen error disc-number encoding (L6); drop 4–7 `MapSizeEstimate` hardwiring (L7) | low |
| M3e | Widen `*`/MiscOp/FileSwitch drive range checks to 0–255 | low |

---

## 5. Open questions
1. Allocate the record table to the **configured** drive count (lean) or always 256 (simple)? — recommend configured.
2. Keep a fast path for ≤ 8 drives (static) and only go dynamic above 8, or always dynamic? — recommend always dynamic (one code path).
3. Does any external client rely on the 3-bit-in-address drive encoding via `FileCore_DiscOp` (legacy)? — needs a compatibility audit before retiring L4 on any path that 3rd parties call.
