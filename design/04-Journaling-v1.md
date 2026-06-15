# FileCore Journaling Hooks — Interface Spec + Reference (M2)

Implements bounty #40's requirement: *"new hooks … to allow 3rd-party modules to journal
changes to the disc … with a list of such changes it would in theory be possible to rewind the
state of the disc to an earlier point."* (See [01-spec §7](01-NewDiscFormat-Spec.md).)

Two parts: (A) the FileCore-side API a real module would integrate, and (B) the host-side
reference that demonstrates the same semantics — including working **rewind** — in `gfctool`.

The hooks are **format-independent**: they sit on FileCore's commit path and work on E+ today
as well as the new G format.

---

## A. FileCore-side interface (for integration)

### Registration SWIs

**`FileCore_RegisterJournal`**
```
In:  R0 = flags   bit0 = deliver before-images
                   bit1 = deliver after-images
                   bit2 = deliver data writes too (else metadata only)
     R1 = handler entry (called in SVC mode)
     R2 = handler R12 (client private word)
Out: R0 = journal client handle
```
**`FileCore_DeregisterJournal`** — `In: R0 = handle`.

Multiple clients may register; FileCore keeps a small list and calls each in turn.

### Discovery service

**`Service_FileCoreJournal`** (non-fatal broadcast) is issued when FileCore starts/initialises
so a module loaded after FileCore can register, and re-issued is harmless. `R2` carries the
FileCore SWI base so the client can call back.

### Handler call

For each journalled event FileCore calls every registered handler:
```
R0 = record type (see below)     R1 = drive (0..255)
R2 = transaction id              R3 = disc address, low 32 (sector)
R4 = disc address, high 32       R5 = length in bytes
R6 = pointer to before-image, or 0
R7 = pointer to after-image, or 0
R12= client private word
Handlers must preserve all registers and must not error (advisory hook).
```

### Record types
| Code | Meaning |
|---|---|
| 0 | `TxnBegin` (R5 = client-data: operation tag) |
| 1 | `TxnCommit` |
| 2 | `MapUpdate` — allocation map sector(s) changed |
| 3 | `MetaWrite` — superblock / AG header / directory metadata |
| 4 | `ObjectWrite` — object record / file data (only if flag bit2) |
| 5 | `Discard` — region freed (TRIM hint) |

### Transaction semantics

FileCore brackets every FS-level mutation (create, delete, rename, extend, truncate, attribute
change, write-behind flush) with `TxnBegin`/`TxnCommit`. Nested internal steps share the one
transaction id, so a client only ever sees **consistent boundaries**. A crash between begin and
commit leaves an incomplete transaction the client can discard on replay.

### Performance / opt-in
- No registered clients ⇒ one flag test on the commit path (near-zero cost).
- Before-/after-images are captured only if some client requested them.
- Data writes are journalled only if a client sets flag bit2; default is metadata-only.

### Rewind (client side)
A client storing before-images keyed by `(drive, seq)` can rewind to an earlier point by
writing the before-images back in **reverse sequence order** down to a chosen `TxnCommit`
boundary (via `FileCore_DiscOp64`). Per-record before-images replayed in reverse correctly undo
even sectors written several times in one session.

---

## B. Host-side reference (`gfctool`)

The reference mirrors the semantics above using a **sidecar journal file** `‹image›.gfcjrnl`.
Mutating commands (currently `mkfile`) run inside a transaction; every sector they overwrite is
captured (magic, seq, txn, before-image) before the write. `rewind` replays before-images.

### Journal file format (little-endian)

Header (32 bytes):
| Off | Sz | Field |
|----|----|-------|
| 0 | 4 | `GFCJ` magic = 0x4A434647 |
| 4 | 2 | version (1) |
| 6 | 2 | reserved |
| 8 | 4 | sector_size |
| 12 | 8 | total_sectors (binds journal to the image) |
| 20 | 8 | next_seq |
| 28 | 4 | reserved |

Records (each starts with a 24-byte common header):
| Off | Sz | Field |
|----|----|-------|
| 0 | 4 | `JREC` magic = 0x4345524A |
| 4 | 8 | seq |
| 12 | 8 | txn |
| 20 | 1 | type (0 begin, 1 commit, 2 write) |
| 21 | 1 | drive |
| 22 | 2 | reserved |

- **begin** `+`: `u16 tag_len`, `tag bytes` (operation description).
- **commit** `+`: nothing.
- **write** `+`: `u64 start_sector`, `u32 nsectors`, `before-image (nsectors × sector_size)`.

### Commands
```
gfctool mkfile  <image> <name> <srcfile>    # now writes a journal transaction
gfctool journal <image>                      # list transactions and records
gfctool rewind  <image> [--to TXN]           # undo last txn (or back to TXN), restore before-images
```
After `rewind`, `check` must still pass and `ls` reflects the earlier state.

> v1 reference stores **before-images only** (sufficient for undo); after-images (for redo) are
> a documented later addition. Journaling covers `mkfile`; `format` establishes the baseline and
> is not journalled.
