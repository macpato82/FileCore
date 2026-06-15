# armcheck — verifier for the FileCore ARM patch arithmetic

Host-side checks for the parts of the (build-unverified) ARM patches that are most prone to
silent corruption.

## macrocheck

Models the exact shift-add/RSB instruction sequences of the patched `DrvRecPtr` / `DiscRecPtr`
macros ([design/06](../../design/06-M3a-DynamicDriveTables.md),
[design/07](../../design/07-M3bc-DriveNumberWidening.md)) and asserts each computes
`base + index × SzRec` for every drive index 0..255 across several base addresses.

```
gcc -std=c99 -O2 -o macrocheck macrocheck.c && ./macrocheck
```
Covers all eight build-flag branches: `DrvRecPtr` Sz = 40/36/24/20, `DiscRecPtr` Sz = 56/48/44/40.
Exits non-zero on any mismatch. This does **not** replace an objasm build — it independently
confirms the address arithmetic of the macro rewrite is correct.
