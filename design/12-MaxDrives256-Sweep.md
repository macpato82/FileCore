# `MaxDrives > 8` — complete change sweep (spec for a >8-capable test pass)

Status: **spec** (execution deferred). Phases A & B of M3b.1 are done and runtime-verified at
`MaxDrives=8` (see below); this document is the precise, reviewable inventory of everything that
remains to actually raise `MaxDrives` past 8, written so it can be executed in one pass against a
test environment that can present >8 drives.

## 0. Why this is a spec, not a patch

The remaining work cannot be validated incrementally in RPCEmu:

- **RPCEmu can't present >8 drives** (one floppy, one IDE channel), so the *>8 behaviour* of every
  change below is untestable here — only the `≤8` (config-8) non-regression path runs.
- **The sentinel change is atomic.** FileCore uses the literal **`8`** as the "no disc / no drive
  attached" value (`DiscsDrv = 8`). If some sites still write `8` while converted sites compare
  against a new sentinel, they disagree **even at `MaxDrives=8`** (a converted reader treats a
  written `8` as "drive 8 attached"). So there is no batch-by-batch ≤8 safety net — it is one
  ~30-site change across ~10 files, and the config-8 floppy test cannot distinguish "all sites
  correct" from "config-8 happens to dodge the bug."
- Several `#8` immediates are **unrelated** (sector-size `;256 byte sectors` at `FileCore20:1395`,
  stack offsets, bit counts) — each must be classified individually, not blanket-replaced.

Hence: execute this as one reviewed pass with a >8 harness, not blind.

## 1. Already done (runtime-verified at `MaxDrives=8`)

- **M3a** — drive/disc record tables relocated to an `OS_Module`-claimed RMA block (`DrvRecsPtr`/
  `DiscRecsPtr`), lifting them out of the `<&1000` workspace. (`patches/m3a.diff`)
- **M3b.0** — Drive Record grown to `SzDrvRec=40` with a reserved `DrvsDiscNum` byte; `DrvRecPtr`
  ×40.
- **M3b.1 Phase A** — disc-record index mirrored into `DrvsDiscNum` at every number-changing write
  (invariant `DrvsDiscNum == DrvsDisc & 7`); index reads source from `DrvsDiscNum`.
- **M3b.1 Phase B** — two standalone has-record tests reworked from `BIC Uncertain; CMP #8/#7` to a
  `TST …,#Unknown:OR:Empty:OR:Full` flag test (`FileCore15` dismount, `FileCore20` density).

The overloaded-byte blocker is therefore gone: the *disc-record index* now lives in a full byte
and has-record is (in those two paths) determined from flags, not the `<8` trick.

## 2. Sentinel scheme decision

`DiscsDrv` (`Defns:120`, 1 byte) currently encodes `0–7 => drive in, 8 => not in drive / disc
record unused`. A byte cannot hold `0–255` **plus** a spare "none". Options:

| Scheme | Drives | Change cost |
|---|---|---|
| **A. `DriveUnattached = &FF`** (recommended) | up to **255** (0–254) | low — value-only; no layout change |
| B. Widen `DiscsDrv` to 16-bit | full 256 | high — `SzDiscRec` + every `LDRB/STRB DiscsDrv` → `H` |
| C. Separate "attached" flag bit in the disc record | full 256 | medium — find a spare bit; touch every attach/detach/test |

**Recommended: Scheme A.** Define `DriveUnattached * &FF` in `Defns`; treat `>= MaxDrives` as
unattached at read sites. Caps usable drives at 255 (document the one-drive reservation; the bounty
target "256" is met to 255, or take Scheme B/C if 256 is mandatory). All comparisons become
`#DriveUnattached`; all writes of the old `8` become `#DriveUnattached`.

## 3. Site inventory

### 3a. Finish the `DrvsDisc → DrvsDiscNum` migration (number reads + has-record)
Remaining reads that still take `DrvsDisc & 7` / `BIC Uncertain` as the index, to convert to
`DrvsDiscNum` + a flag-based has-record test:
- `InitDieSvc:930–955` — the disc↔drive consistency loop. Reads `DrvsDisc` at `:939` and `:945`
  (`TEQ`/`CMP` against the disc-record number / `#8`). **Trap:** a no-record drive has
  `DrvsDiscNum=0`, which must *not* be read as "points at record 0" — gate on the flag test first.
- `FileCore20:2453–2457` — `UnlinkCommon`'s local caller: `LDRB R2,[R1,#DrvsDisc]; BIC R2,#Uncertain`
  → `TST` flags, `LDREQB R2,[R1,#DrvsDiscNum]`, else `MOV R2,#DriveUnattached`.
- `Commands:463` — `LDRB r1,[r1,#DrvsDisc]; BL ActiveDismountDisc` → has-record gate + `DrvsDiscNum`.

### 3b. `DiscsDrv` sentinel (`8` → `DriveUnattached`)
Writers of the old `8`:
- `FileCore20:2483–2484` (`MOV R2,#8; STRB R2,[LR,#DiscsDrv]`) — `UnlinkCommon`.
- (audit any other `MOV …,#8; STRB …,#DiscsDrv`).
Comparators against `8` (confirmed `DiscsDrv`-coupled):
- `Commands:773–774`, `Commands:2571–2576`, `Commands:768` (`MOV R1,#8; BL CloseAllByDisc`).
- `FileCore20:132–133`, `FileCore20:2173–2174`, `FileCore20:2467` (`UnlinkCommon`).
- `FileCore80:2885–2886`, `FileCore80:5399–5400`, `:5405`, `:5421–5422`.
Other `DiscsDrv` reads to audit for a nearby `#8` (each verified individually):
`Commands:1982/2024/2141/2517/2518`, `FileCore15:628`, `FileCore33:1127`, `FileCore35:81/104/1047/1106/1153/1391`,
`FileCore40:839`, `FileCore60:1047/1250/1408/1702/1754`, `FileCore70:594/625/1282`, `FileCore80:1088/3229`.
Update the `Defns:120` comment and the `FileCore20:2398` `ASSERT DiscsDrv=SzDiscRec-2` (still holds).

### 3c. `UnlinkCommon` contract
`FileCore20:2463–2468`: `CMPS R2,#8; Pull HS` (bail if "no attachment"). Change to
`CMPS R2,#DriveUnattached`. **Audit every caller** of `UnlinkCommon` / `UnlinkByDisc` /
`UnlinkByDrive` so the value they pass for "none" is `DriveUnattached`, not `8`.

### 3d. Hardcoded `0..7` / `#8` iteration bounds
- `InitDieSvc:930` (`MOV R0,#7` — iterate disc records 0..7) and similar loops → derive from
  `MaxDrives`/`MaxDiscRecs`. Audit `DebugOpts:175/196` (debug, low priority).
- The init record-table loop already uses `R7 = Floppies+4` for drive existence — confirm nothing
  else assumes exactly 8.

### 3e. M3c — drive number in disc addresses (the `DiscBits` field)
Every FileCore disc address carries the disc/drive number in its **top 3 bits**:
`DiscBits = 2_111 :SHL: (32-3)` (`Defns:59`), used at ~40 sites (`#DiscBits` masks and
`LSR/ASL #(32-3)` (un)packs across `Commands`, `BigDirCode`, `FileCore15/20/30/35`, …).
- **Do not widen `DiscBits` in place** (it would touch the on-disc/handle address format everywhere).
- Instead route wide drives via **DiscOp64**, which already carries an **8-bit drive byte**
  separately (`GenSWIs` DiscOp64 dispatch builds it). `GenIndDiscOp` currently packs the drive into
  the 3-bit field (`LSR/ORR #(32-3)`) — for drives `≥8`, carry the drive out-of-band to
  `DoDiscOp`/DiscOp64 and keep the 3-bit legacy path for `≤8`.
- `GenSWIs:400–401` (`CMP LR,#8; BHS DO64_BadDrive`) is itself an 8-drive cap **in the DiscOp64
  path** — raise to `MaxDrives`.
- `FcbIndDiscAdd` (file handles, e.g. `FileCore20:446` `TEQ r2, r0, LSR #(32-3)`) stores the 3-bit
  disc number; wide discs need the number carried alongside or the handle format extended.

### 3f. `MaxDrives` bump + RMA size
- `Defns:277` `MaxDrives * 8` → desired (`255` for Scheme A, or `256`).
- RMA record block = `MaxDrives*(SzDrvRec+SzDiscRec)` = `255*(40+56)` ≈ **24 KB** (was 768 B) —
  fine for RMA; confirm the `MOV R3,#…` immediate is encodable (use `LDR =` if not).
- Tables are in RMA (M3a), so the `<&1000` workspace limit is unaffected.

### 3g. Clients (outside FileCore)
ADFS / SCSIFS must issue DiscOp64 (8-bit drive) and present >8 drives for any of this to exercise
at runtime. Their drive-config (`Floppies`/`Winnies` packed counts passed to `FileCore_Create`,
`DriveConfig_*` masks) also assume ≤8 and need widening. This is separate-component work.

## 4. Test plan (requires a >8 environment)
1. `≤8` non-regression: configured-8 mount/read/write/dismount unchanged (the only part testable in
   RPCEmu — run after the atomic change to catch gross breakage).
2. A harness presenting `>8` drives (real hardware, or an emulator built to expose >8): mount discs
   on drives `8…N`, exercise attach/detach/unlink, cross-drive ops, and the disc↔drive consistency
   pass at init; verify no aliasing of the old `8` sentinel.
3. Disc-address round-trip for drive `≥8` through DiscOp64 (3e).

## 5. Risk
The change is atomic and individually-classified across ~30 sites with no >8 runtime feedback. The
dominant risk is a *missed* sentinel/loop/address site that silently aliases at >8. Execution should
diff every `#8` and every `DiscsDrv`/`DiscBits`/`(32-3)` site against this inventory before building,
and run plan §1 immediately after to catch config-8 regressions.
