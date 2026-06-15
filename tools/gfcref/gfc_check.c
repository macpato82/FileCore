/*
 * gfc_check.c - check-byte algorithms.
 *
 * gfc_zone_check : port of map_zone_valid_byte from FileCore Doc/EMaps.
 * gfc_dir_check  : directory check byte from FileCore Doc/Dirs.
 * gfc_struct_check: 32-bit word sum for the new G superblock / AG header.
 */
#include "gfc.h"

/*
 * Port of map_zone_valid_byte (Doc/EMaps). Operates on one zone-sized
 * window. Byte 0 of the zone is excluded so the returned value can be
 * stored there as the check byte. Here `zone` already points at the
 * start of the zone sector, so zone_start is 0.
 */
uint8_t gfc_zone_check(const uint8_t *zone, uint32_t sector_size)
{
    uint32_t s0=0,s1=0,s2=0,s3=0;
    int32_t rover;
    for (rover = (int32_t)sector_size - 4; rover > 0; rover -= 4) {
        s0 += zone[rover+0] + (s3>>8); s3 &= 0xff;
        s1 += zone[rover+1] + (s0>>8); s0 &= 0xff;
        s2 += zone[rover+2] + (s1>>8); s1 &= 0xff;
        s3 += zone[rover+3] + (s2>>8); s2 &= 0xff;
    }
    /* rover == 0: skip byte 0 (the check byte itself) */
    s0 +=             (s3>>8);
    s1 += zone[1]   + (s0>>8);
    s2 += zone[2]   + (s1>>8);
    s3 += zone[3]   + (s2>>8);
    return (uint8_t)((s0^s1^s2^s3) & 0xff);
}

/* 32-bit little-endian word sum over [0, sector_size-4). */
uint32_t gfc_struct_check(const uint8_t *sector, uint32_t sector_size)
{
    uint32_t sum = 0, i;
    for (i = 0; i + 4 <= sector_size - 4; i += 4)
        sum += get_u32(sector + i);
    return sum;
}

/*
 * Directory check byte (Doc/Dirs): accumulate used bytes with
 * EOR r0, r1, r0, ROR #13, then EOR the four bytes of the result.
 * The very last word (holding the check byte) is excluded.
 *
 * Simplified, well-defined v1 variant operating byte-wise over the
 * used region excluding the final byte; documented in design/02.
 */
static uint32_t ror32(uint32_t v, int n){ return (v>>n)|(v<<(32-n)); }

uint8_t gfc_dir_check(const uint8_t *dir, size_t used_len, size_t total_len)
{
    uint32_t acc = 0;
    size_t i;
    (void)total_len;
    /* accumulate all used bytes except the final check byte slot */
    for (i = 0; i + 1 < used_len; i++)
        acc = (uint32_t)dir[i] ^ ror32(acc, 13);
    return (uint8_t)((acc ^ (acc>>8) ^ (acc>>16) ^ (acc>>24)) & 0xff);
}
