/*
 * macrocheck.c - verify the address arithmetic of the patched FileCore
 * DrvRecPtr / DiscRecPtr macros (M3a / M3b, design/06 & design/07).
 *
 * Each macro computes  Rptr = dynamic_base + Rindex * SzRec  using a fixed
 * sequence of shift-add (and one RSB) ARM ops. A wrong shift would silently
 * point at the wrong record -> disc corruption. This models each patched
 * sequence exactly and asserts it equals base + Rindex*Sz for every index
 * 0..255 (256-drive range) and a range of bases.
 *
 * Pure host C (gcc), runs anywhere. Returns non-zero on any mismatch.
 */
#include <stdio.h>
#include <stdint.h>

typedef uint32_t u32;

/* --- DrvRecPtr branches (base then accumulate Rindex*Sz) --- */
/* SzDrvRec=40 (M3b): Rptr=Rindex*5 ; Rptr=base+(Rptr<<3)  -> *40 */
static u32 drv40(u32 base,u32 i){ u32 r=i+(i<<2); return base+(r<<3); }
/* SzDrvRec=36 (M3a DynamicMaps): Rptr=Rindex*9 ; base+(Rptr<<2) -> *36 */
static u32 drv36(u32 base,u32 i){ u32 r=i+(i<<3); return base+(r<<2); }
/* SzDrvRec=24 (BigDisc): Rptr=Rindex*3 ; base+(Rptr<<3) -> *24 */
static u32 drv24(u32 base,u32 i){ u32 r=i+(i<<1); return base+(r<<3); }
/* SzDrvRec=20: Rptr=Rindex*5 ; base+(Rptr<<2) -> *20 */
static u32 drv20(u32 base,u32 i){ u32 r=i+(i<<2); return base+(r<<2); }

/* --- DiscRecPtr branches --- */
/* SzDiscRec=56 (BigShare+BigDir): Rptr=Rindex*7 via RSB ; base+(Rptr<<3) -> *56 */
static u32 disc56(u32 base,u32 i){ u32 r=(i<<3)-i; return base+(r<<3); }
/* SzDiscRec=48 (BigShare,!BigDir): Rptr=Rindex*3 ; base+(Rptr<<4) -> *48 */
static u32 disc48(u32 base,u32 i){ u32 r=i+(i<<1); return base+(r<<4); }
/* SzDiscRec=44 (!BigShare): Rptr=Rindex*11 ; base+(Rptr<<2) -> *44 */
static u32 disc44(u32 base,u32 i){ u32 r=i+(i<<1); r=r+(i<<3); return base+(r<<2); }
/* SzDiscRec=40 (!BigDisc): Rptr=Rindex*5 ; base+(Rptr<<3) -> *40 */
static u32 disc40(u32 base,u32 i){ u32 r=i+(i<<2); return base+(r<<3); }

struct { const char *name; u32 (*fn)(u32,u32); u32 sz; } cases[] = {
    {"DrvRecPtr  Sz=40", drv40,40}, {"DrvRecPtr  Sz=36", drv36,36},
    {"DrvRecPtr  Sz=24", drv24,24}, {"DrvRecPtr  Sz=20", drv20,20},
    {"DiscRecPtr Sz=56", disc56,56},{"DiscRecPtr Sz=48", disc48,48},
    {"DiscRecPtr Sz=44", disc44,44},{"DiscRecPtr Sz=40", disc40,40},
};

int main(void){
    u32 bases[] = {0, 0x1000, 0x8000, 0x01C02000u, 0xFFFF0000u};
    int fails=0, n=sizeof cases/sizeof cases[0];
    for (int c=0;c<n;c++){
        u32 bad=0;
        for (unsigned b=0;b<sizeof bases/sizeof bases[0];b++)
            for (u32 i=0;i<256;i++){
                u32 want = bases[b] + i*cases[c].sz;   /* mod 2^32, matches ARM */
                u32 got  = cases[c].fn(bases[b], i);
                if (got!=want){ bad++; if(bad==1)
                    printf("  MISMATCH %s base=%08x i=%u want=%08x got=%08x\n",
                           cases[c].name,bases[b],i,want,got); }
            }
        printf("%-18s : %s\n", cases[c].name, bad? "FAIL":"ok (256 indices x 5 bases)");
        fails += (bad!=0);
    }
    if (fails){ printf("\nMACROCHECK FAILED: %d branch(es)\n",fails); return 1; }
    printf("\nMACROCHECK OK: all 8 macro branches compute base + index*SzRec exactly\n");
    return 0;
}
