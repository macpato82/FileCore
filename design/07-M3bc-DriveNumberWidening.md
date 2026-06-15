# M3b + M3c — Drive-Number Widening to 256 (design + patch)

Builds on [M3a](06-M3a-DynamicDriveTables.md). M3a made the record *tables* dynamic but kept
`MaxDrives = 8` (behaviour-preserving). This step lifts the two remaining 3-bit caps so the
table can actually grow past 8:

- **M3b** — widen the `DrvsDisc` disc-record *index* (Drive Record) from 3 bits to a full byte.
- **M3c** — stop packing the drive into the **top 3 bits of the disc address**; carry it separately
  (8-bit), routing through the DiscOp64 path.

M3b and M3c are presented together because **neither delivers value alone** — both 3-bit fields
must widen, and `MaxDrives` be raised, in the same change. (This is why M3b was not done standalone
after M3a: with the address format still 3-bit, a wider `DrvsDisc` index addresses drives the
DiscOp path can't reach.)

> **Status: design + build-unverified patch.** Against v3.78. The record/macro edits are exact and
> hand-verified; the per-site address-routing sweep is specified and must be completed under an
> objasm/RPCEmu build loop (it is broad and individually mechanical). Usable-drive increase also
> needs the low-level FS (ADFS/SCSIFS) to support `DiscOp64`.

---

## 1. The three drive-number encodings (recap, grounded)

| Encoding | Width | Where | 256-ready? |
|---|---|---|---|
| DiscOp64 address block drive byte | **8 bits** | `ExtendedDiscAddress_DriveNumber` (`hdr/FileCore:148`); read at `GenSWIs:385-454` | ✅ already |
| Legacy disc-address packed drive | **3 bits** (`<<29`) | `GenIndDiscOp` `FileCore15:395,401` (`LSR/ORR #(32-3)`, mask `DiscBits`) → `DoDiscOp` | ❌ M3c |
| Drive Record disc index (`DrvsDisc` b0-2) | **3 bits** | `s/Defns:154-159`; used as `DiscRecPtr` index e.g. `FileCore15:396` | ❌ M3b |

`FileCore_Create` R3 counts are already 8-bit (`Defns:82-85`), and the in-memory "current drive"
that indexes `DrvRecs` is already a full value (dynamic table after M3a). The caps are purely these
two 3-bit fields plus the low-level DiscOp address format.

---

## 2. M3b — widen the `DrvsDisc` index

**Constraint:** `SzDrvRec = 36` (DynamicMaps) and `DrvFlags`' spare bits *could* hold the 4 state
flags, but the cleanest, lowest-churn option keeps the **state-flag sites untouched** and adds a
dedicated index byte, growing the record to a macro-friendly size.

### 2.1 Drive Record (`s/Defns`)
Keep `DrvsDisc` for the **state flags only** (Uncertain/Unknown/Empty/Full, bits 4-7); add a
full-byte index and pad to keep word sizing:
```
DrvsDisc        # 1     ; state flags only now (Uncertain/Unknown/Empty/Full)
DrvsDiscNum     # 1     ; disc-record index 0..MaxDrives-1 (was DrvsDisc bits 0-2)
                # 2     ; pad -> SzDrvRec 36->40 (macro-friendly: *5<<3)
```
> Alternative considered: fold the 4 state flags into `DrvFlags` spare bits (0 size growth) — but
> that touches ~20 flag-test sites (`FileCore20:72,91,549`, `FileCore15:723`, `Commands:2449` …).
> Adding a byte keeps those untouched; only the *number* sites change. Net lower risk.

### 2.2 `DrvRecPtr` macro (`s/MyMacros`) — SzDrvRec 36 → 40
The M3a `DrvRecPtr` (DynamicMaps branch) becomes:
```
        ASSERT  SzDrvRec=40
        ADD$cond $Rptr, $Rindex, $Rindex, LSL #2   ; Rindex*5
        Push    "SB"
        LDR$cond SB, [SB, #DrvRecsPtr]
        ADD$cond $Rptr, SB, $Rptr, LSL #3          ; dynbase + Rindex*40
        Pull    "SB"
```
`5·8 = 40 ✓`, alias- and condition-safe (as M3a). The 24/20 legacy branches are unaffected (those
build flags imply the smaller record without the new byte).

### 2.3 Sites: number vs flags
- **Number sites** (the only ones that change) — anywhere the disc-record index was read from or
  written to `DrvsDisc`'s low 3 bits, e.g. the value feeding `DiscRecPtr` derived from `DrvsDisc`,
  and `STRB`/`ORR` that set the number (`FileCore20:470`, identify/mount paths). Change to read/write
  **`DrvsDiscNum`** and drop the `:AND: 7` masking.
- **Flag sites** (unchanged) — every `TST DrvsDisc,#Uncertain|Empty|Unknown|Full` and
  `MOV #Uncertain:OR:Unknown; STRB …DrvsDisc` keeps working (those bits stay in `DrvsDisc`).

---

## 3. M3c — carry the drive separately, not in the address top bits

### 3.1 The choke point (`GenIndDiscOp`, `FileCore15:389-409`)
Current (old-map / big-disc path):
```
        MOV     R6,R2,LSR #(32-3)       ; drive = top 3 bits of disc address
        DiscRecPtr R7,R6                ; disc record for that drive
        LDRB    R7,[R7,#DiscRecord_Log2SectorSize]
        BIC     R2,R2,#DiscBits         ; strip drive bits
        MOV     R2,R2,LSR R7            ; byte -> sector
        ADD     R2,R2,R5,LSR R7
        ORR     R2,R2,R6,LSL #(32-3)    ; drive back into top 3 bits
        BL      DoDiscOp
```
The drive is squeezed into bits 29-31 of the 32-bit `R2` handed to `DoDiscOp` — the 8-drive cap.

### 3.2 Change
- Source the drive from the **current-drive context** (the value already used to select the Drive
  Record, full 8-bit) rather than re-deriving it from the address top bits; keep it in its **own
  register** alongside a **64-bit sector** address.
- Replace the `ORR … LSL #(32-3)` packing + `DoDiscOp` with the **DiscOp64 dispatch** (drive byte +
  64-bit sector), the same form `GenSWIs:385-454` already builds for the `FileCore_DiscOp64` SWI.
- `DoDiscOp` (and the low-level `FS_DiscOp` veneer) gains/uses a 64-bit + separate-drive entry. For
  drives 0-7 on media whose low-level FS lacks DiscOp64, fall back to the legacy 3-bit packing —
  preserving exact current behaviour (backwards compatible).

### 3.3 Dependencies
- The low-level filing system (ADFS/SCSIFS) must implement `DiscOp64` for drives > 7. This is the
  same `DiscOp64` already defined; M3c is FileCore *using* it on the internal path, not just exposing
  the SWI.
- Couples with the format work: G-format addresses are already 64-bit sector + AG (design/02), so the
  internal `(drive, 64-bit sector)` representation is the natural fit.

---

## 4. Sequencing & flags

1. Land M3b (record/macro/number-sites) and M3c (GenIndDiscOp/DoDiscOp routing) together.
2. Raise `MaxDrives` (e.g. to 256) **only after** both, and after the low-level FS supports DiscOp64.
3. Guard the wide path behind a feature/Create flag so a config with ≤ 8 drives and a legacy
   low-level FS takes the unchanged 3-bit path — zero behavioural change there.

## 5. Verification (build loop)
- objasm builds for the default and legacy flag sets; `SzDrvRec=40` asserts hold; `<&1000` still OK.
- RPCEmu: mount/read/write/dismount on ≤ 8 drives (legacy path) unchanged.
- With a DiscOp64-capable low-level FS and `MaxDrives>8`: exercise a drive index ≥ 8 end-to-end.
- Confirm `DrvsDiscNum` and the state-flag bits never alias (grep the number-site sweep is complete).
