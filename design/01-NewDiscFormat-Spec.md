# FileCore Large-Disc Format — Design Specification (Draft 1)

**Bounty:** RISC OS Open #40 — "Add a new format to the native filing systems for large discs"
**Component:** `RiscOS/Sources/FileSys/FileCore` (analysed at v3.78, 15 Apr 2023)
**Status:** Draft for discussion. Nothing here is committed; this is the first bounty deliverable (the design).
**Author:** (working draft)

---

## 1. Scope and requirements

From the bounty text, the deliverable is a new native disc format that:

1. Supports **up to 256 drives per filing system** (ADFS/SCSIFS/etc.).
2. Supports **up to 16 EB (2^64 bytes) per drive**.
3. Is **tailored to RISC OS** — must preserve load/exec addresses, file-type information, attributes, and the FileCore object model so that existing FileSwitch semantics are unchanged.
4. Adds **journaling hooks** — a service-call / registration scheme letting 3rd-party modules record every change to the disc, with enough information that disc state can in theory be rewound to an earlier point.
5. Has a **recovery story** worked out *with* 3rd-party disc-tool vendors before release (DiscKnight, etc.).
6. Internally extends FileCore per **DiscOp64** — 64-bit sector addressing, drive number broken out into its own byte.

The design decision the bounty explicitly asks us to make first: **extend E+ again, adopt a foreign OS layout, or design a custom layout.** Section 4 answers this; Sections 2–3 justify the answer.

---

## 2. What already exists (do not re-do)

FileCore 3.78 already contains most of the *plumbing* groundwork. The bounty is **not** a green-field filesystem; it is the next format generation on top of substantial existing work.

| Capability | State in 3.78 | Source evidence |
|---|---|---|
| 64-bit DiscOp transport | **Present.** `FileCore_DiscOp64` SWI + extended address block (drive byte + 64-bit sector address). | `hdr/FileCore:53,146-152` |
| 64-bit disc size in disc record | **Present.** `DiscSize` (low 32) + `BigMap_DiscSize2` (high 32). | `hdr/FileCore:173,184` |
| Big files (4 GB, full FileSwitch range) | **Present.** 33-/64-bit internal extent maths. | `Doc/BigFiles` |
| Big maps (idlen widened to 21 bits) | **Present.** `BigMap_*` fields; commit "Increase upper range of idlen to 21 bits". | `hdr/FileCore:184-192`, git log |
| Big directories | **Present** (`BigDir` format, `s/BigDirCode`). | `hdr/FileCore:193-194` |
| 2K/4K sector sizes | **Present.** | git log "Add support for 2k and 4k sector sizes" |
| Disc record format version field | **Present.** `BigDir_DiscVersion`. | `hdr/FileCore:193` |

**Implication.** The 64-bit *addressing* path (DiscOp64) and the 64-bit *size* fields exist. What does **not** exist, and what this bounty must deliver, is:

- An **on-disc allocation structure that actually scales to 16 EB** (see §3 — the current zone bitmap cannot).
- **256-drive** support through FileCore's internal drive tables and the legacy 3-bit drive field.
- **Journaling hooks** (nothing in the tree).
- A **format/layout/recovery** toolchain for the new format.

---

## 3. Why E+ cannot reach 16 EB — the bitmap scaling wall

The E+ map (`Doc/EMaps`) is a **single flat bitmap of the whole disc**:

- The disc is divided into `nzones` zones; the map is `nzones` sectors long, one map sector per zone.
- Each zone contributes ≈ `sector_size × 8` allocation bits.
- Each allocation bit covers `BPMB = 2^log2bpmb` bytes ("bytes per map bit").
- Therefore:

```
disc_size  =  total_map_bits × BPMB
           ≈  nzones × sector_size × 8 × BPMB
```

Hard ceilings on the factors:

| Factor | Field | Max | Note |
|---|---|---|---|
| `nzones` | `NZones` + `BigMap_NZones2` | 2^16 = 65 535 | 8+8 bits |
| `sector_size` | `Log2SectorSize` | 4 096 B | log2 = 12 |
| `idlen` (object-id width) | `IdLen` | 21 bits | ⇒ ≤ ~2.1M objects/disc |

So `total_map_bits ≈ 65535 × 4096 × 8 ≈ 2^31`. Two consequences:

**(a) Granularity blows up.** To make `disc_size = 2^64`, you need `BPMB = 2^64 / 2^31 = 2^33 = 8 GB per map bit`. The Large File Allocation Unit is `(idlen+1) × BPMB ≈ 22 × 8 GB ≈ 176 GB`. Every file rounds up to ~176 GB. Useless.

With a *sane* LFAU of, say, 64 KB (`BPMB ≈ 4 KB`), the same `2^31` bits cap the disc at `2^31 × 4 KB = 8 TB`. **E+ tops out around 8 TB at usable granularity** — roughly four orders of magnitude short of 16 EB.

**(b) The map itself becomes unmanageable.** At `2^31` bits the flat map is **256 MB**. FileCore caches the map and walks free-chains in it (`Doc/EMaps`, `s/FileCore33`). A quarter-gigabyte flat structure that must be consulted for every allocation is a non-starter — even before you reach the granularity wall.

**Object-id width** is a third, softer wall: `idlen ≤ 21` ⇒ ~2.1M objects per disc. A 16 EB disc holding millions-to-billions of files needs ≥ 32–48 bit object ids.

**Conclusion:** a single flat full-disc bitmap is structurally incapable of 16 EB. The allocation structure *must* change. This rules out "just extend E+ again."

---

## 4. Design decision: custom layout reusing the FileCore model ("F+ / G format")

Three options, assessed against the requirements:

### Option A — Extend E+ once more
**Rejected.** §3 shows the flat bitmap cannot scale in granularity *or* metadata size. No amount of field-widening fixes an O(disc_size) flat map.

### Option B — Adopt a foreign on-disc format (exFAT, ext4, XFS, ZFS, …)
**Rejected as the base format**, retained as design inspiration. Foreign formats:
- have no native place for RISC OS load/exec/attributes/file-type (would need a sidecar/xattr scheme — fragile, breaks the recovery story);
- discard FileCore's object-id indirection and existing directory/recovery code;
- are very large to implement *correctly* in ARM assembler (ZFS/XFS especially);
- complicate the vendor recovery-tool coordination the bounty mandates.
- *However:* their **allocation-group** idea (ext4 block groups, XFS allocation groups) is exactly the fix for §3 and is adopted below.

### Option C — Custom layout that keeps the FileCore object model — **RECOMMENDED**

Call it **FileCore "G" format** (next letter after E/F; "F+" if we prefer to signal continuity). Keep everything that already works and is RISC-OS-shaped:

- the **disc record / boot block** superblock concept (`Doc/Discs`, `Doc/Formats`);
- the **object-id → fragment-list** indirection (so directories still store a compact `IndDiscAdd`, recovery from directories still works);
- the **directory format** (BigDir) with load/exec/attrs/file-type unchanged;
- the **per-zone bitmap + free-chain** machinery (`s/FileCore33`, `FormSWIs`) — *reused unchanged inside each allocation group.*

…and break the §3 ceiling with **two structural changes**:

1. **Allocation Groups (AGs).** Partition the disc into N allocation groups, each ≤ a bounded size (e.g. 4–256 GB, format-time choice). **Each AG carries its own self-contained E+-style zone bitmap** sized for that AG only. The full-disc map never exists; only the AG maps for AGs you are touching are loaded/cached. This:
   - caps any single map at the per-AG size (megabytes, not gigabytes);
   - makes metadata size scale with *AGs touched*, not disc size;
   - **reuses the existing, tested zone/fragment/free-chain code per-AG** — the single biggest risk reducer.
   - To reach 16 EB: e.g. 64 GB AGs × 2^28 AGs = 2^64. AG count is a 32–40 bit field in the superblock.

2. **Wide object ids and 64-bit addressing.** Object id becomes a `(AG number, intra-AG id)` pair, or a flat ≥40-bit id. `IndDiscAdd` is widened (new format already passes drive separately under DiscOp64, freeing bits). All internal disc addresses are 64-bit **sector** numbers fed straight to `FileCore_DiscOp64`.

This is the lowest-risk path that actually hits the target: it is mostly **composition of existing FileCore mechanisms at a new scale**, plus a new top-level AG index, rather than a new filesystem.

> Open question for ROOL/vendors: AGs vs. a global **extent B-tree** (XFS-style) for free space. AGs reuse far more existing code (recommended for v1); a B-tree gives better large-contiguous allocation but is a much bigger asm effort and a new recovery story. Recommend **AGs for the first format**, leaving a superblock feature-flag for a future B-tree free-space option.

---

## 5. On-disc layout (G format) — proposed

```
LBA 0 ............ Boot block + primary superblock (extended disc record, 64-bit)
                   + AG directory (table of AG base sectors / sizes / map locations)
LBA n ............ Secondary superblock copy (recovery)
AG 0 ............. [ AG header | AG zone-bitmap | objects (dirs/files) ]
AG 1 ............. [ AG header | AG zone-bitmap | objects ]
...
AG k-1 ........... ...
(optional) ....... Journal control area (if internal journaling chosen; see §7)
```

### 5.1 Extended superblock (disc record v-next)
Built on the existing disc record (`hdr/FileCore:156-197`). New/changed fields:

| Field | Width | Purpose |
|---|---|---|
| `DiscVersion` | 4 | Bumped to G-format magic/version. Old tools see an unknown version and refuse (safe). |
| `DiscSize` (sectors, 64-bit) | 8 | Total size in **sectors** (with `Log2SectorSize`), not bytes — removes the byte-count ambiguity noted in `s/Identify`. |
| `AGCount` | 4–5 | Number of allocation groups. |
| `Log2AGSize` | 1 | AG size = 2^n sectors (all AGs equal except possibly the last). |
| `Log2bpmb` / `IdLen` | per-AG | Now interpreted per AG (uniform across AGs in v1). |
| `ObjIdWidth` | 1 | Bits of object id (≥ 40). |
| `FeatureFlags` | 4 | Bitfield: journaling-present, B-tree-freespace, etc. Lets future formats negotiate. |
| Two superblock copies | — | Primary + secondary for recovery (as F format already keeps two map copies). |

Reserve generous padding for forward compatibility; keep the **first `SzDiscRecSig` bytes byte-compatible** with the legacy disc record so existing identify code can read version/sector-size and bail cleanly on G discs it shouldn't touch.

### 5.2 Allocation Group
- **AG header:** check bytes, this-AG number, free-space summary (free clusters, largest free run — for fast allocation target selection), pointer to AG map.
- **AG map:** an E+ zone bitmap exactly as `Doc/EMaps` describes, but sized for the AG. **Existing map code (`FileCore33`, `FormSWIs`, defect handling) operates within one AG with minimal change** — it just needs the zone base offset to be the AG base rather than 0.
- **Objects:** files/dirs allocated from this AG's free space. An object whose data spills beyond one AG is represented by fragments in multiple AGs, chained the way E+ already chains cross-zone fragments (`Doc/EMaps` object-ordering rules) — generalised one level up to AGs.

### 5.3 Directory & object model — unchanged
BigDir directory format is retained verbatim (load/exec/len/attrs/file-type all preserved). Only the embedded disc address widens. This keeps the **"recover the tree by walking directories" story** intact and keeps FileSwitch semantics identical.

---

## 6. 256-drive support

DiscOp64's address block already carries an 8-bit `DriveNumber` (`hdr/FileCore:148`). The remaining work is internal:

- Audit every place the **legacy 3-bit drive field** in `IndDiscAdd` is assumed (`Doc/Think/BigDiscs` lists `bits 29-31 = disc number`). Under G format, in-memory object addresses must carry the full 8-bit drive separately (DiscOp64 already does this on the wire).
- Widen FileCore's **per-drive tables / DriveCB arrays** (currently sized for the historical 4 floppy + 4 winnie model) to 256 entries, or make them sparse.
- `*` commands, `MiscOp`, mount/dismount, and the FileSwitch drive-name mapping need to accept drive numbers 0–255.

This is mechanical but wide-reaching; it should be a **separate milestone** (M-Drives) that can land independently of the new map, since DiscOp64 already defines the interface.

---

## 7. Journaling hooks

Nothing exists yet; this is green-field and the most self-contained piece. Goal (bounty wording): *"new hooks … to allow 3rd party modules to journal changes to the disc … with a list of such changes it would in theory be possible to rewind the state of the disc to an earlier point."*

**Design: before-image change records via registration + service call.**

1. **Registration.** New SWIs `FileCore_RegisterJournal` / `FileCore_DeregisterJournal` (and a mirror `Service_FileCoreJournal` broadcast so modules can discover an already-running FileCore). A client registers a coroutine/handler + a private word.

2. **Change records.** Before FileCore commits any *metadata or data* mutation that changes persistent disc state, it calls each registered handler with a record:

   ```
   { drive, sequence#, txn#, op-type, disc-address (64-bit sector),
     length, before-image-ptr (optional), after-image-ptr (optional) }
   ```

   `op-type` covers: map alloc/free, directory write, superblock/AG-header write, object create/delete, data overwrite. Supplying the **before-image** is what makes *rewind* possible — the client stores old contents and can replay them backwards.

3. **Transactions.** FileCore brackets multi-step operations (e.g. "create file" = map alloc + dir write + parent update) with `txn-begin` / `txn-commit` markers so a journal client can only ever rewind to a **consistent** boundary, never mid-operation. This reuses the consistency intent already implicit in `StartMasSeq/EndMasSeq` (`Doc/Dirs`) and the map cross-check (`Doc/EMaps`).

4. **Performance / opt-in.** Zero registered clients ⇒ near-zero cost (one "any clients?" test on the commit path). Before-image capture only happens if a client asks for it (a registration flag), so the common case stays cheap.

5. **Where the journal lives** is the *client's* problem, not FileCore's — could be a reserved on-disc area (declared via a superblock feature flag + AG-directory entry), a separate disc, or RAM. This keeps FileCore format-agnostic about journal storage and lets vendors innovate.

This hook design is **independent of the new map format** and could be prototyped/shipped against E+ first, de-risking it early.

---

## 8. Recovery, compatibility, and vendor coordination

- **Old tools must not corrupt G discs.** The bumped `DiscVersion` + a distinct boot-block signature ensure legacy FileCore/ADFS/DiscKnight either ignore or politely refuse a G disc. The legacy-compatible prefix of the disc record (§5.1) guarantees they can at least *read the version and stop*.
- **Two superblock copies + per-AG check bytes + cross-checks** preserve the F-format "two copies / self-checking map" recovery property (`Doc/Formats`) at AG granularity.
- **Recovery from directories** still works because the directory format and object-id indirection are retained (§5.3).
- **Vendor engagement is a gating task, not an afterthought** (bounty requirement). Produce: (a) this spec, (b) an on-disc format reference with check-byte algorithms (reuse the documented `NewCheck` / `map_zone_valid_byte` from `Doc/EMaps`), (c) sample images, and circulate to DiscKnight et al. *before* freezing the format.

---

## 9. Tooling

`Tools/c` already contains a C disc-format toolset (`Dirs`, `EMaps`, `Reclaim`, `disccheck`) that parses E+ structures off-target. **Extend this C toolset to the G format first** — a host-side formatter + checker + (de)serialiser is the cheapest place to validate the layout, generate sample images for vendors, and build a reference implementation before committing to ARM assembler. This mirrors the existing `Test/Tester` harness approach.

---

## 10. Proposed milestones (independently shippable)

| # | Milestone | Depends on | Ships value alone? |
|---|---|---|---|
| M0 | This spec + format reference + vendor circulation | — | Yes (bounty deliverable 1) |
| M1 | C reference implementation: G-format formatter + checker + sample images (`Tools/c`) | M0 | Yes (validation + vendor artefacts) |
| M2 | Journaling hooks (register/service-call/txn) on **existing E+** | — (parallel) | Yes (usable on today's discs) |
| M3 | 256-drive internal support (drive tables, legacy 3-bit field) | DiscOp64 (done) | Yes |
| M4 | G-format read support in FileCore (mount/identify/read AGs) | M1 | Partial |
| M5 | G-format write/allocate (per-AG map reuse, cross-AG objects) | M4 | Yes |
| M6 | Format/layout SWIs + integration with ADFS/SCSIFS | M5 | Yes |
| M7 | Journaling wired to G-format metadata path | M2, M5 | Yes |

M1, M2, M3 can proceed in parallel and each delivers standalone value, matching how this codebase historically de-risked BigDisc/BigFiles/BigMap as separate switches.

---

## 11. Open questions (need ROOL / vendor input)

1. **AGs vs. extent B-tree** for free space (§4). Recommend AGs for v1.
2. **AG size** policy — fixed at format time, or adaptive? Recommend fixed `Log2AGSize`.
3. **Object-id width** and whether ids are flat or `(AG, local)`.
4. **Journal storage** — does FileCore reserve an on-disc area, or leave it 100% to the client? (Recommend client-owned, FileCore only emits records.)
5. **Format letter/name** — "G" vs "F+" vs marketing name.
6. **Minimum sector size** for G discs — mandate 512 B floor, or allow the format only on ≥ 4 KB media?
7. Backwards conversion: is in-place E+ → G upgrade required, or copy-only?

---

*Next action after sign-off on direction: M1 — build the C reference formatter/checker so the layout can be validated and sample images handed to recovery-tool vendors.*
