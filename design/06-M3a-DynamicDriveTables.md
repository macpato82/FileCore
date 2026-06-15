# M3a — Dynamic Drive/Disc Record Tables (implementation patch)

First implementation step of [M3](05-256Drives-v1.md). Relocates the static `DrvRecs`/`DiscRecs`
workspace arrays into a dynamically-claimed block addressed via a pointer, removing the
`ASSERT :INDEX:{VAR}<&1000` (4 KB) workspace blocker and the hard-wired `* 8`.

**Scope / risk:** *behaviour-preserving refactor.* The table is sized by a named constant
`MaxDrives` (currently **8**, unchanged), so the module behaves identically — it is verifiable as
"still works with 8 drives." Raising `MaxDrives` and removing the 3-bit address packing
(M3c) is what then unlocks more drives; M3a only removes the storage blocker.

> **Status: build-unverified.** Written against `RiscOS/Sources/FileSys/FileCore` v3.78. Needs an
> objasm build + RPCEmu smoke test (see §6). All address arithmetic below is hand-verified.

---

## 1. New constant (`s/Defns`)

Add near the map limits (e.g. after `MaxIdLenBigMap`):
```
MaxDrives       * 8     ; size of the drive/disc record tables (was a literal 8)
```

## 2. Workspace (`s/FileCore00`)

**Before** (lines 273-275):
```
                ; Records
DrvRecs         # SzDrvRec * 8
DiscRecs        # SzDiscRec * 8
```
**After** — replace the in-line arrays with pointers (16 bytes instead of ~800):
```
                ; Records (now dynamically allocated; see InitDieSvc)
DrvRecsPtr      # 4     ; -> block of MaxDrives drive records
DiscRecsPtr     # 4     ; -> block of MaxDrives disc records
```
The `Dev` debug lines 302-303 that print `:INDEX:DrvRecs/DiscRecs` should print the new
`:INDEX:DrvRecsPtr/DiscRecsPtr` instead.

## 3. Indexing macros (`s/MyMacros`) — the centralised core

Every indexed access funnels through `DiscRecPtr`/`DrvRecPtr`. Each branch currently ends:
```
        ADD$cond $Rptr, <Rindex multiply> ...     ; Rptr = SB + Rindex*Sz
        ...
        MinOps  ADD, ADD, $Rptr, $Rptr, (:INDEX:DiscRecs), $cond   ; + static array offset
```
We keep each branch's proven multiply but change the **base** from `SB + (:INDEX:…)` to the loaded
pointer. The pointer is loaded into `SB` across a balanced `Push/Pull "SB"` — `SB` is then used
exactly as before as the base register, and the original workspace base is restored afterwards.

This is **alias-safe** (`$Rptr == $Rindex` permitted: `$Rindex` is consumed in the first multiply
op, as in the original) and **condition-safe** (the `Push/Pull` are unconditional and balanced; the
`LDR`/`ADD` carry `$cond`, so a false condition leaves `SB` and `$Rptr` unchanged).

### 3.1 `DiscRecPtr` — replace the tail of each `SzDiscRec` branch
Drop the final `MinOps … (:INDEX:DiscRecs)` line and wrap the base-add. Pattern per branch
(`k1`,`k2` = the branch's existing shifts; `multiply` = its existing first op):

```
        <existing first multiply op, e.g.  ADD$cond $Rptr,$Rindex,$Rindex,ASL #k1 >
        Push    "SB"
        LDR$cond SB, [SB, #DiscRecsPtr]
        ADD$cond $Rptr, SB, $Rptr, ASL #k2
        Pull    "SB"
```
Concrete per active build flag (`Sz = base + Rindex*Sz`, verified):

| Flags | SzDiscRec | first op | k2 | check |
|---|---|---|---|---|
| BigShare+BigDir | 56 | `RSB $Rptr,$Rindex,$Rindex,ASL #3` (×7) | 3 | 7·8 = 56 ✓ |
| BigShare, !BigDir | 48 | `ADD $Rptr,$Rindex,$Rindex,ASL #1` (×3) | 4 | 3·16 = 48 ✓ |
| !BigShare | 44 | ×11 (keep original 2-op incl. `$Rptr<>$Rindex` split) | 2 | 11·4 = 44 ✓ |
| !BigDisc | 40 | `ADD $Rptr,$Rindex,$Rindex,ASL #2` (×5) | 3 | 5·8 = 40 ✓ |

(For the ×11 / SzDiscRec=44 branch keep the existing `[ $Rptr<>$Rindex … | Push "SB" … ]`
multiply that yields `$Rptr = Rindex*11`, then apply the same `Push "SB" / LDR SB,[SB,#DiscRecsPtr]
/ ADD $Rptr,SB,$Rptr,ASL #2 / Pull "SB"` base-add.)

### 3.2 `DrvRecPtr` — same transformation
| Flags | SzDrvRec | first op | k2 | check |
|---|---|---|---|---|
| DynamicMaps | 36 | `ADD $Rptr,$Rindex,$Rindex,LSL #3` (×9) | 2 | 9·4 = 36 ✓ |
| BigDisc, !DynamicMaps | 24 | `ADD $Rptr,$Rindex,$Rindex,LSL #1` (×3) | 3 | 3·8 = 24 ✓ |
| else | 20 | `ADD $Rptr,$Rindex,$Rindex,LSL #2` (×5) | 2 | 5·4 = 20 ✓ |

Drop the trailing `MinOps … (:INDEX:DrvRecs)`; wrap the second op with `Push "SB" / LDR$cond SB,
[SB,#DrvRecsPtr] / ADD$cond $Rptr,SB,$Rptr,LSL #k2 / Pull "SB"`.

## 4. Allocation & init (`s/InitDieSvc`)

`R7` = floppy count, `R8` = fixed count are already parsed (lines 69-72). Before the record init
loop (line 342), claim one RMA block of `MaxDrives*(SzDrvRec+SzDiscRec)` bytes and split it:

```
;allocate drive & disc record tables (replaces static workspace arrays)
        MOV     R3, #MaxDrives*(SzDrvRec+SzDiscRec)
        MOV     R0, #ModHandReason_Claim          ; OS_Module 6
        SWI     XOS_Module
        BVS     <Create fails: propagate error>
        STR     R2, DrvRecsPtr                    ; drive records first
        ADD     LR, R2, #MaxDrives*SzDrvRec
        STR     LR, DiscRecsPtr                   ; disc records follow
```
Then change the loop base loads (lines 342-343):
```
        sbaddr  R4, DrvRecs      ->   LDR  R4, DrvRecsPtr
        sbaddr  R5, DiscRecs     ->   LDR  R5, DiscRecsPtr
```
The existing loop body (init each record, `ADD R4,R4,#SzDrvRec` / `ADD R5,R5,#SzDiscRec`,
counter to 8) is unchanged except the iteration count `8` → `MaxDrives`. Zero the whole block
first (or rely on the existing per-field init, which already writes every field).

## 5. Free on Die + direct sites

- **Die** (instance teardown in `s/InitDieSvc`, the `Die` path ~line 595+): before releasing
  workspace, `LDR R2, DrvRecsPtr` and `OS_Module 7` (Free) it (guard against 0). The disc table
  shares the same block, so one free suffices.
- **Direct `sbaddr` sites** (must load the pointer instead):
  - `s/Commands:2395` `sbaddr R4, DiscRecs+DiscRecord_DiscName` → `LDR R4, DiscRecsPtr` then
    `ADD R4, R4, #DiscRecord_DiscName`.
  - `s/FileCore40:901` — same pattern.
  - `s/InitDieSvc:342-343` — handled in §4.
- All `CritDrvRec`/`CritDiscRec` users are **unaffected** — they hold an already-resolved record
  pointer, not an array base.

## 6. Verification checklist (needs the RISC OS toolchain)

> The macro address arithmetic (§3) is **machine-verified** host-side by
> [`tools/armcheck/macrocheck`](../tools/armcheck/README.md) — all branches compute
> `base + index*SzRec` for indices 0..255.
>
> **Phase 1 build-verified (2026): the pointerised macros assemble and link cleanly** with
> objasm 4.13 in RPCEmu (RISC OS 5 / IOMD, APCS-32) — unmodified→patched FileCore both produce
> `rm.IOMD.FileCore` with 0 errors. The verified diff is [`patches/m3a-phase1.diff`](../patches/m3a-phase1.diff).
> Phase 1 keeps the static tables and only routes access through `DrvRecsPtr`/`DiscRecsPtr`
> (behaviour-identical). Phase 2 (below) swaps the tables for a dynamically-claimed block.


1. `objasm` builds clean for the default flag set (BigDisc+BigDir+BigShare+DynamicMaps ⇒ SzDiscRec=56,
   SzDrvRec=36) and for a legacy flag set.
2. `ASSERT :INDEX:{VAR}<&1000` still holds (now with headroom).
3. RPCEmu (`N:\rpcemu_*`): boots, mounts a FileCore disc, reads/writes/deletes, `*Dismount`,
   re-mounts — i.e. all `DiscRecPtr`/`DrvRecPtr` paths exercised with the dynamic table.
4. No leak across module reinit/Die (claim/free balanced).
5. Optional: temporarily set `MaxDrives` higher and confirm the build/asserts still pass (proves the
   table is no longer workspace-bound) — usable drive count remains gated until M3c.
