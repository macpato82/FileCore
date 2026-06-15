/*
 * gfc.h - FileCore "G" format reference: on-disc constants and helpers.
 *
 * Host-side reference implementation for RISC OS Open bounty #40.
 * All on-disc fields are little-endian. See design/02-GFormat-OnDisc-v1.md.
 */
#ifndef GFC_H
#define GFC_H

#include <stdint.h>
#include <stddef.h>

/* ---- magics / versions ---- */
#define GFC_SB_MAGIC       0x31434647u   /* "GFC1" LE */
#define GFC_AGH_MAGIC      0x47414647u   /* "GFAG" LE */
#define GFC_DISC_VERSION   0x47314644u   /* distinct value so legacy tools refuse */
#define GFC_FMT_MAJOR      1
#define GFC_FMT_MINOR      0

#define GFC_DISCTYPE_DATA  0xFFD         /* FileType_Data */
#define GFC_OBJID_LOCALBITS 15

/* ---- superblock field offsets ---- */
#define SB_Log2SectorSize   0
#define SB_IdLen            4
#define SB_Log2bpmb         5
#define SB_NZones           9
#define SB_ZoneSpare        10
#define SB_Root             12
#define SB_DiscSize         16
#define SB_DiscId           20
#define SB_DiscName         22
#define SB_DiscType         32
#define SB_DiscSize2        36
#define SB_BigMapFlags      41
#define SB_NZones2          42
#define SB_DiscVersion      44
#define SB_RootDirSize      48
#define SB_GFC_MAGIC        64
#define SB_FmtMajor         68
#define SB_FmtMinor         70
#define SB_TotalSectors     72
#define SB_AGCount          80
#define SB_Log2AGSize       88
#define SB_Log2SecSizeEcho  89
#define SB_MapZones         90
#define SB_FeatureFlags     92
#define SB_ObjIdAGBits      96
#define SB_ObjIdLocalBits   97
#define SB_RootObjId        100
#define SB_DataStartInAG    108
#define SB_ClustersPerAG    112
#define SB_Log2SecsPerClu   116
#define SB_DiscNameLen      10

/* ---- AG header field offsets ---- */
#define AGH_Magic           0
#define AGH_AGNumber        4
#define AGH_BaseSector      12
#define AGH_SizeSectors     20
#define AGH_HeaderSector    28
#define AGH_MapStartSector  36
#define AGH_MapZones        44
#define AGH_DataStartSector 48
#define AGH_ClustersTotal   56
#define AGH_ClustersFree    64
#define AGH_LargestFreeRun  72

/* ---- object record (start of an object's first cluster) ---- */
#define GFC_OBJ_MAGIC       0x424F4647u   /* "GFOB" LE */
#define OBJ_HDR_BYTES       64
#define OBJ_TYPE_FILE       1
#define OBJ_TYPE_DIR        2
#define OBJ_Magic           0
#define OBJ_ObjId           4
#define OBJ_Type            12
#define OBJ_Attrs           13
#define OBJ_Load            16
#define OBJ_Exec            20
#define OBJ_Length          24
#define OBJ_StartSector     32
#define OBJ_ClusterCount    40
#define OBJ_Name            48
#define OBJ_NameLen         10
#define OBJ_HdrCheck        60

/* ---- directory contents (follow the object record in a dir object) ---- */
#define DIRENT_BYTES        40
#define DE_Name             0
#define DE_NameLen          12
#define DE_Type             12
#define DE_Attrs            13
#define DE_Load             16
#define DE_Exec             20
#define DE_Length           24
#define DE_StartSector      32

/* ---- map zone header ---- */
#define ZONE_HDR_BYTES      4   /* ZoneCheck, CrossCheck, reserved[2] */

/* ---- little-endian accessors ---- */
static inline void put_u16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static inline void put_u32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static inline void put_u64(uint8_t *p, uint64_t v){ put_u32(p,(uint32_t)v); put_u32(p+4,(uint32_t)(v>>32)); }
static inline uint16_t get_u16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static inline uint32_t get_u32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline uint64_t get_u64(const uint8_t *p){ return (uint64_t)get_u32(p)|((uint64_t)get_u32(p+4)<<32); }

/* ---- geometry computed from format parameters ---- */
typedef struct {
    uint32_t sector_size;
    uint8_t  log2_sector_size;
    uint8_t  log2_bpmb;          /* cluster = 1<<log2_bpmb bytes */
    uint8_t  log2_agsize;        /* AG = 1<<log2_agsize sectors  */
    uint8_t  log2_secs_per_clu;  /* log2_bpmb - log2_sector_size */
    uint64_t total_sectors;
    uint64_t agcount;
    uint32_t agsize_sectors;     /* nominal AG size in sectors   */
    uint32_t clusters_per_ag;    /* nominal clusters in a full AG */
    uint16_t map_zones;          /* map zones per AG             */
    uint32_t bits_per_zone;      /* allocation bits per map zone */
    uint8_t  objid_ag_bits;
} gfc_geom;

/* check-byte algorithms (gfc_check.c) */
uint8_t  gfc_zone_check(const uint8_t *zone, uint32_t sector_size);
uint32_t gfc_struct_check(const uint8_t *sector, uint32_t sector_size);
uint8_t  gfc_dir_check(const uint8_t *dir, size_t used_len, size_t total_len);

#endif /* GFC_H */
