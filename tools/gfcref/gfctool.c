/*
 * gfctool.c - FileCore "G" format reference: formatter, checker, info.
 *
 * RISC OS Open bounty #40. Host-side reference; see design/02-GFormat-OnDisc-v1.md.
 *
 *   gfctool format <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR]
 *   gfctool check  <image>
 *   gfctool info   <image>
 *
 * Sizes accept K/M/G/T/E suffixes (powers of 1024).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "gfc.h"

/* 64-bit file offsets, portably (MinGW lacks fseeko by default) */
#if defined(_WIN32)
  #define gfc_fseek(f,off,w) _fseeki64((f),(long long)(off),(w))
  #define gfc_ftell(f)       _ftelli64(f)
#else
  #define gfc_fseek(f,off,w) fseeko((f),(off_t)(off),(w))
  #define gfc_ftell(f)       ftello(f)
#endif

/* ---- journaling (host-side reference of the FileCore hooks) ---- */
#define JRNL_MAGIC 0x4A434647u   /* "GFCJ" */
#define JREC_MAGIC 0x4345524Au   /* "JREC" */
#define JREC_BEGIN 0
#define JREC_COMMIT 1
#define JREC_WRITE 2
#define JREC_HDR_BYTES 24
typedef struct {
    FILE *jf;            /* sidecar journal file */
    FILE *imgf;          /* image being journalled */
    const void *g;       /* gfc_geom* */
    uint64_t txn, seq;
    uint8_t  drive;
    int active;
} journal_t;
static journal_t J = {0};
static void jcapture_before(FILE *f, const void *g, uint64_t sec);  /* fwd */

static int is_pow2_u64(uint64_t v){ return v && (v & (v-1))==0; }
static uint8_t log2_u64(uint64_t v){ uint8_t n=0; while(v>1){v>>=1;n++;} return n; }
static uint64_t ceil_div(uint64_t a, uint64_t b){ return (a + b - 1) / b; }

static int parse_size(const char *s, uint64_t *out)
{
    char *end; errno=0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end==s) return -1;
    uint64_t mul = 1;
    if (*end){
        switch(*end){
            case 'k': case 'K': mul=1024ull; break;
            case 'm': case 'M': mul=1024ull*1024; break;
            case 'g': case 'G': mul=1024ull*1024*1024; break;
            case 't': case 'T': mul=1024ull*1024*1024*1024; break;
            case 'e': case 'E': mul=1024ull*1024*1024*1024*1024*1024; break;
            default: return -1;
        }
        if (end[1]) return -1;
    }
    *out = (uint64_t)v * mul;
    return 0;
}

/* ---- geometry ---- */
static int compute_geom(uint64_t total_bytes, uint32_t sector_size,
                        uint32_t cluster, uint32_t agsize_sectors,
                        gfc_geom *g, char *err, size_t errsz)
{
    memset(g,0,sizeof *g);
    if (!is_pow2_u64(sector_size) || sector_size<512 || sector_size>4096){
        snprintf(err,errsz,"sector size must be a power of two in 512..4096"); return -1; }
    if (!is_pow2_u64(cluster) || cluster<sector_size){
        snprintf(err,errsz,"bpmb (cluster) must be a power of two >= sector size"); return -1; }
    if (!is_pow2_u64(agsize_sectors)){
        snprintf(err,errsz,"ag-size (sectors) must be a power of two"); return -1; }
    if (total_bytes % sector_size){
        snprintf(err,errsz,"total size must be a multiple of the sector size"); return -1; }

    g->sector_size      = sector_size;
    g->log2_sector_size = log2_u64(sector_size);
    g->log2_bpmb        = log2_u64(cluster);
    g->log2_secs_per_clu= (uint8_t)(g->log2_bpmb - g->log2_sector_size);
    g->log2_agsize      = log2_u64(agsize_sectors);
    g->agsize_sectors   = agsize_sectors;
    g->total_sectors    = total_bytes / sector_size;
    g->bits_per_zone    = sector_size*8 - ZONE_HDR_BYTES*8;
    g->clusters_per_ag  = agsize_sectors >> g->log2_secs_per_clu;
    if (g->clusters_per_ag == 0){
        snprintf(err,errsz,"ag-size smaller than one cluster"); return -1; }
    g->map_zones        = (uint16_t)ceil_div(g->clusters_per_ag, g->bits_per_zone);

    /* AG must hold header + map + at least one data cluster */
    {
        uint32_t reserved_secs = 1 /*hdr*/ + g->map_zones;
        if (g->agsize_sectors <= reserved_secs + (1u<<g->log2_secs_per_clu)){
            snprintf(err,errsz,"ag-size too small for metadata + data"); return -1; }
    }
    g->agcount = ceil_div(g->total_sectors, agsize_sectors);
    if (g->agcount == 0){ snprintf(err,errsz,"empty disc"); return -1; }
    g->objid_ag_bits = g->agcount<=1 ? 1 : (uint8_t)(log2_u64(g->agcount-1)+1);
    return 0;
}

static uint64_t ag_base(const gfc_geom *g, uint64_t i){ return i << g->log2_agsize; }
static uint64_t ag_header_sector(const gfc_geom *g, uint64_t i){ return i==0 ? 2 : ag_base(g,i); }
static uint64_t ag_map_start(const gfc_geom *g, uint64_t i){ return ag_header_sector(g,i)+1; }
static uint64_t ag_data_start(const gfc_geom *g, uint64_t i){ return ag_map_start(g,i)+g->map_zones; }
static uint64_t ag_real_sectors(const gfc_geom *g, uint64_t i){
    uint64_t base=ag_base(g,i), rem=g->total_sectors-base;
    return rem < g->agsize_sectors ? rem : g->agsize_sectors;
}
static uint64_t ag_real_clusters(const gfc_geom *g, uint64_t i){
    return ag_real_sectors(g,i) >> g->log2_secs_per_clu;
}
/* first data cluster index within the AG (clusters before it are header+map[+SBs]) */
static uint32_t ag_first_data_cluster(const gfc_geom *g, uint64_t i){
    uint64_t span = ag_data_start(g,i) - ag_base(g,i);
    return (uint32_t)ceil_div(span, (uint64_t)1<<g->log2_secs_per_clu);
}
/* usable (object-holding) clusters in AG i: real clusters minus structural (header+map,
 * SBs in AG0) minus the root cluster in AG0. */
static uint64_t ag_usable(const gfc_geom *g, uint64_t i){
    uint64_t real=ag_real_clusters(g,i), resv=ag_first_data_cluster(g,i)+(i==0?1:0);
    return real>resv ? real-resv : 0;
}
/* total usable clusters disc-wide, computed analytically (O(1), works at 16 EB scale) */
static uint64_t total_usable_clusters(const gfc_geom *g){
    uint64_t n=g->agcount;
    if (n==1) return ag_usable(g,0);
    uint64_t full = (n>=2) ? (n-2) : 0;                 /* AGs 1..n-2 are full-size */
    return ag_usable(g,0) + full*ag_usable(g,1) + ag_usable(g,n-1);
}

/* ---- map bit access ---- */
static void map_set(uint8_t *map, const gfc_geom *g, uint32_t c){
    uint32_t z=c/g->bits_per_zone, b=c%g->bits_per_zone;
    map[z*g->sector_size + ZONE_HDR_BYTES + b/8] |= (uint8_t)(1u<<(b&7));
}
static int map_get(const uint8_t *map, const gfc_geom *g, uint32_t c){
    uint32_t z=c/g->bits_per_zone, b=c%g->bits_per_zone;
    return (map[z*g->sector_size + ZONE_HDR_BYTES + b/8] >> (b&7)) & 1;
}
static void map_clear(uint8_t *map, const gfc_geom *g, uint32_t c){
    uint32_t z=c/g->bits_per_zone, b=c%g->bits_per_zone;
    map[z*g->sector_size + ZONE_HDR_BYTES + b/8] &= (uint8_t)~(1u<<(b&7));
}

/*
 * Mark the reserved/allocated clusters for AG i and return (via *free_out)
 * the number of free real clusters and (*total_out) real clusters.
 * Reserved = header+map (and SBs in AG0), root cluster (AG0), tail padding.
 */
static void ag_reserved_bits(uint8_t *map, const gfc_geom *g, uint64_t i,
                             int with_root, uint64_t *free_out, uint64_t *total_out)
{
    uint32_t first_data = ag_first_data_cluster(g,i);
    uint64_t real_clu   = ag_real_clusters(g,i);
    uint32_t c, alloc_real=0;

    for (c=0; c<first_data; c++){           /* header + map (+SBs in AG0) */
        if (c < real_clu){ if(!map_get(map,g,c)) alloc_real++; }
        map_set(map,g,c);
    }
    if (i==0 && with_root){                  /* root directory cluster */
        if (first_data < real_clu){ if(!map_get(map,g,first_data)) alloc_real++; map_set(map,g,first_data); }
    }
    for (c=(uint32_t)real_clu; c<g->clusters_per_ag; c++) map_set(map,g,c); /* tail padding */

    if (total_out) *total_out = real_clu;
    if (free_out)  *free_out  = real_clu - alloc_real;
}

/* fill zone check bytes for a fully-populated map buffer */
static void map_finalise_checks(uint8_t *map, const gfc_geom *g)
{
    uint16_t z;
    for (z=0; z<g->map_zones; z++){
        uint8_t *zs = map + (uint32_t)z*g->sector_size;
        zs[2]=zs[3]=0;
        zs[1]= (z==g->map_zones-1) ? 0xFF : 0x00;   /* CrossCheck: EOR == 0xFF */
        zs[0]=0;
        zs[0]= gfc_zone_check(zs, g->sector_size);
    }
}

/* ---- objects & directories ---- */
static uint32_t sum_words(const uint8_t *p, uint32_t n){ uint32_t s=0,i; for(i=0;i+4<=n;i+=4) s+=get_u32(p+i); return s; }

/* fill the 64-byte object record at the start of a cluster buffer */
static void build_obj_header(uint8_t *c, uint64_t objid, uint8_t type, uint8_t attrs,
                             uint16_t extent_count, uint32_t load, uint32_t exec, uint64_t length,
                             uint64_t start_sector, uint64_t data_clusters, const char *name)
{
    memset(c,0,OBJ_HDR_BYTES);
    put_u32(c+OBJ_Magic,GFC_OBJ_MAGIC);
    put_u64(c+OBJ_ObjId,objid);
    c[OBJ_Type]=type; c[OBJ_Attrs]=attrs;
    put_u16(c+OBJ_ExtentCount,extent_count);
    put_u32(c+OBJ_Load,load); put_u32(c+OBJ_Exec,exec);
    put_u64(c+OBJ_Length,length);
    put_u64(c+OBJ_StartSector,start_sector);
    put_u64(c+OBJ_ClusterCount,data_clusters);
    memset(c+OBJ_Name,' ',OBJ_NameLen);
    { size_t n=strlen(name); if(n>OBJ_NameLen)n=OBJ_NameLen; memcpy(c+OBJ_Name,name,n); }
    put_u32(c+OBJ_HdrCheck, sum_words(c,OBJ_HdrCheck));
}

/* build an empty root directory object into a freshly zeroed cluster buffer */
static void build_root_object(uint8_t *c, uint32_t cluster_size, uint64_t objid, uint64_t start_sector){
    memset(c,0,cluster_size);
    build_obj_header(c,objid,OBJ_TYPE_DIR,0,0,0,0,4,start_sector,1,"$");
    put_u32(c+OBJ_HDR_BYTES,0);   /* EntryCount = 0 */
}

/* ---- cross-AG cluster allocation ----
 * absolute sector <-> (AG, local cluster) helpers and a multi-AG allocator. */
typedef struct { uint64_t start_sector, cluster_count; } extent_t;

static uint64_t sec_to_ag(const gfc_geom *g, uint64_t sec){ return sec >> g->log2_agsize; }
static uint32_t sec_to_localclu(const gfc_geom *g, uint64_t sec){
    uint64_t ag=sec_to_ag(g,sec); return (uint32_t)((sec-(ag<<g->log2_agsize))>>g->log2_secs_per_clu);
}
static uint64_t localclu_to_sec(const gfc_geom *g, uint64_t ag, uint32_t lc){
    return (ag<<g->log2_agsize) + ((uint64_t)lc<<g->log2_secs_per_clu);
}

/* ---- superblock ---- */
static void build_superblock(uint8_t *sb, const gfc_geom *g, uint64_t total_bytes,
                             const char *name, uint32_t root_local, uint64_t usable, int lazy)
{
    memset(sb,0,g->sector_size);
    sb[SB_Log2SectorSize]=g->log2_sector_size;
    sb[SB_IdLen]=15;
    sb[SB_Log2bpmb]=g->log2_bpmb;
    sb[SB_NZones]=(uint8_t)g->map_zones;
    put_u32(sb+SB_DiscSize,(uint32_t)total_bytes);
    memset(sb+SB_DiscName,' ',SB_DiscNameLen);
    { size_t n=strlen(name); if(n>SB_DiscNameLen)n=SB_DiscNameLen; memcpy(sb+SB_DiscName,name,n); }
    put_u32(sb+SB_DiscType,GFC_DISCTYPE_DATA);
    put_u32(sb+SB_DiscSize2,(uint32_t)(total_bytes>>32));
    sb[SB_BigMapFlags]=1;                       /* BigFlag */
    sb[SB_NZones2]=(uint8_t)(g->map_zones>>8);
    put_u32(sb+SB_DiscVersion,GFC_DISC_VERSION);
    put_u32(sb+SB_RootDirSize,OBJ_HDR_BYTES+4);  /* empty root object */
    put_u32(sb+SB_GFC_MAGIC,GFC_SB_MAGIC);
    put_u16(sb+SB_FmtMajor,GFC_FMT_MAJOR);
    put_u16(sb+SB_FmtMinor,GFC_FMT_MINOR);
    put_u64(sb+SB_TotalSectors,g->total_sectors);
    put_u64(sb+SB_AGCount,g->agcount);
    sb[SB_Log2AGSize]=g->log2_agsize;
    sb[SB_Log2SecSizeEcho]=g->log2_sector_size;
    put_u16(sb+SB_MapZones,g->map_zones);
    put_u32(sb+SB_FeatureFlags, lazy?FEATURE_LAZY_AG:0);
    put_u64(sb+SB_TotalClusters,usable);
    put_u64(sb+SB_FreeClusters, usable?usable-1:0);   /* root consumes one usable cluster */
    sb[SB_ObjIdAGBits]=g->objid_ag_bits;
    sb[SB_ObjIdLocalBits]=GFC_OBJID_LOCALBITS;
    put_u64(sb+SB_RootObjId,(uint64_t)root_local);   /* AG 0 */
    put_u32(sb+SB_DataStartInAG,(uint32_t)(1+g->map_zones));
    put_u32(sb+SB_ClustersPerAG,g->clusters_per_ag);
    sb[SB_Log2SecsPerClu]=g->log2_secs_per_clu;
    put_u32(sb+g->sector_size-4, gfc_struct_check(sb,g->sector_size));
}

static void build_ag_header(uint8_t *h, const gfc_geom *g, uint64_t i,
                            uint64_t cfree, uint64_t ctotal)
{
    memset(h,0,g->sector_size);
    put_u32(h+AGH_Magic,GFC_AGH_MAGIC);
    put_u64(h+AGH_AGNumber,i);
    put_u64(h+AGH_BaseSector,ag_base(g,i));
    put_u64(h+AGH_SizeSectors,ag_real_sectors(g,i));
    put_u64(h+AGH_HeaderSector,ag_header_sector(g,i));
    put_u64(h+AGH_MapStartSector,ag_map_start(g,i));
    put_u16(h+AGH_MapZones,g->map_zones);
    put_u64(h+AGH_DataStartSector,ag_data_start(g,i));
    put_u64(h+AGH_ClustersTotal,ctotal);
    put_u64(h+AGH_ClustersFree,cfree);
    put_u64(h+AGH_LargestFreeRun,cfree);   /* contiguous after format */
    put_u32(h+g->sector_size-4, gfc_struct_check(h,g->sector_size));
}

/* ---- low-level image IO ---- */
static int write_sector(FILE *f, const gfc_geom *g, uint64_t sec, const uint8_t *buf){
    if (J.active && f==J.imgf) jcapture_before(f,g,sec);   /* journal before-image */
    if (gfc_fseek(f, sec*g->sector_size, SEEK_SET)) return -1;
    return fwrite(buf,g->sector_size,1,f)==1 ? 0 : -1;
}
static int read_sector(FILE *f, const gfc_geom *g, uint64_t sec, uint8_t *buf){
    if (gfc_fseek(f, sec*g->sector_size, SEEK_SET)) return -1;
    return fread(buf,g->sector_size,1,f)==1 ? 0 : -1;
}
static int read_run(FILE *f, const gfc_geom *g, uint64_t start_sec, uint64_t clusters, uint8_t *buf){
    uint64_t s, nsec=clusters<<g->log2_secs_per_clu;
    for (s=0;s<nsec;s++) if (read_sector(f,g,start_sec+s,buf+s*g->sector_size)) return -1;
    return 0;
}
static int write_run(FILE *f, const gfc_geom *g, uint64_t start_sec, uint64_t clusters, const uint8_t *buf){
    uint64_t s, nsec=clusters<<g->log2_secs_per_clu;
    for (s=0;s<nsec;s++) if (write_sector(f,g,start_sec+s,buf+s*g->sector_size)) return -1;
    return 0;
}
/* scan AG0 map for free count and largest contiguous free run, within real clusters */
static void map_scan_free(const uint8_t *map, const gfc_geom *g, uint64_t real_clu,
                          uint64_t *free_out, uint64_t *largest_out){
    uint64_t c, free=0, run=0, largest=0;
    for (c=0;c<real_clu;c++){
        if (map_get(map,g,(uint32_t)c)==0){ free++; if(++run>largest) largest=run; }
        else run=0;
    }
    *free_out=free; *largest_out=largest;
}
/* global free-cluster counter, from the superblock (O(1), scales to 16 EB) */
static uint64_t total_free_clusters(FILE *f, const gfc_geom *g){
    uint8_t *p=malloc(g->sector_size); read_sector(f,g,0,p);
    uint64_t v=get_u64(p+SB_FreeClusters); free(p); return v;
}
/* adjust the global free-cluster counter in both superblock copies */
static void sb_adjust_free(FILE *f, const gfc_geom *g, int64_t delta){
    uint8_t *p=malloc(g->sector_size); read_sector(f,g,0,p);
    put_u64(p+SB_FreeClusters, get_u64(p+SB_FreeClusters)+(uint64_t)delta);
    put_u32(p+g->sector_size-4, gfc_struct_check(p,g->sector_size));
    write_sector(f,g,0,p); write_sector(f,g,1,p);
    free(p);
}
/* load AG i's map into `map`, lazily initialising the AG (header+reserved map) if needed */
static void ag_init_map(FILE *f, const gfc_geom *g, uint64_t i, uint8_t *map){
    uint8_t hdr[4096];
    if (read_sector(f,g,ag_header_sector(g,i),hdr)==0 && get_u32(hdr+AGH_Magic)==GFC_AGH_MAGIC){
        for (uint16_t z=0;z<g->map_zones;z++) read_sector(f,g,ag_map_start(g,i)+z,map+(uint32_t)z*g->sector_size);
        return;
    }
    uint64_t cfree,ctot;
    memset(map,0,(size_t)g->map_zones*g->sector_size);
    ag_reserved_bits(map,g,i,(i==0),&cfree,&ctot);
    map_finalise_checks(map,g);
    { uint8_t *h=malloc(g->sector_size); build_ag_header(h,g,i,cfree,ctot); write_sector(f,g,ag_header_sector(g,i),h); free(h); }
    for (uint16_t z=0;z<g->map_zones;z++) write_sector(f,g,ag_map_start(g,i)+z,map+(uint32_t)z*g->sector_size);
}

/* Allocate `need` clusters across AGs (first-fit), emitting one extent per
 * contiguous run. Persists each touched AG's map + header. Returns 0 / -1. */
static int alloc_clusters(FILE *f, const gfc_geom *g, uint64_t need,
                          extent_t *ext, int *n_ext, int max_ext)
{
    uint8_t *map=malloc((size_t)g->map_zones*g->sector_size);
    uint8_t *h=malloc(g->sector_size);
    *n_ext=0;
    for (uint64_t i=0;i<g->agcount && need>0;i++){
        uint64_t real=ag_real_clusters(g,i);
        uint32_t first=ag_first_data_cluster(g,i) + (i==0?1:0); /* skip root in AG0 */
        ag_init_map(f,g,i,map);                                /* read or lazily init */
        int touched=0;
        uint32_t c=first;
        while (c<real && need>0){
            if (map_get(map,g,c)){ c++; continue; }
            uint32_t run_start=c, run=0;
            while (c<real && need>0 && !map_get(map,g,c)){ map_set(map,g,c); run++; c++; need--; touched=1; }
            if (*n_ext>=max_ext){ free(map); free(h); return -1; }
            ext[*n_ext].start_sector=localclu_to_sec(g,i,run_start);
            ext[*n_ext].cluster_count=run; (*n_ext)++;
        }
        if (touched){
            map_finalise_checks(map,g);
            uint64_t fr,lg; map_scan_free(map,g,real,&fr,&lg);
            for (uint16_t z=0;z<g->map_zones;z++) write_sector(f,g,ag_map_start(g,i)+z,map+(uint32_t)z*g->sector_size);
            read_sector(f,g,ag_header_sector(g,i),h);
            put_u64(h+AGH_ClustersFree,fr); put_u64(h+AGH_LargestFreeRun,lg);
            put_u32(h+g->sector_size-4,gfc_struct_check(h,g->sector_size));
            write_sector(f,g,ag_header_sector(g,i),h);
        }
    }
    free(map); free(h);
    return need==0 ? 0 : -1;
}

/* Free the given cluster sectors (clearing map bits), per affected AG.
 * Persists each touched AG's map + header. */
static void free_clusters(FILE *f, const gfc_geom *g, const uint64_t *secs, int n)
{
    uint8_t *map=malloc((size_t)g->map_zones*g->sector_size), *h=malloc(g->sector_size);
    for (uint64_t i=0;i<g->agcount;i++){
        int touched=0;
        for (int j=0;j<n;j++) if (sec_to_ag(g,secs[j])==i){
            if (!touched){ ag_init_map(f,g,i,map); touched=1; }
            map_clear(map,g,sec_to_localclu(g,secs[j]));
        }
        if (touched){
            map_finalise_checks(map,g);
            uint64_t fr,lg; map_scan_free(map,g,ag_real_clusters(g,i),&fr,&lg);
            for (uint16_t z=0;z<g->map_zones;z++) write_sector(f,g,ag_map_start(g,i)+z,map+(uint32_t)z*g->sector_size);
            read_sector(f,g,ag_header_sector(g,i),h);
            put_u64(h+AGH_ClustersFree,fr); put_u64(h+AGH_LargestFreeRun,lg);
            put_u32(h+g->sector_size-4,gfc_struct_check(h,g->sector_size));
            write_sector(f,g,ag_header_sector(g,i),h);
        }
    }
    free(map); free(h);
}

/* ---- directory navigation (paths use '/' ; maps to RISC OS '.') ---- */

/* find `name` in directory at dir_sec; returns 1 + fills out_sec/out_type */
static int dir_find(FILE *f, const gfc_geom *g, uint64_t dir_sec, const char *name,
                    uint64_t *out_sec, uint8_t *out_type){
    uint32_t cb=1u<<g->log2_bpmb; uint8_t *d=calloc(1,cb);
    read_run(f,g,dir_sec,1,d);
    uint32_t n=get_u32(d+OBJ_HDR_BYTES); int found=0;
    for (uint32_t e=0;e<n;e++){
        const uint8_t *de=d+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        if(!strcmp(nm,name)){ if(out_sec)*out_sec=get_u64(de+DE_StartSector); if(out_type)*out_type=de[DE_Type]; found=1; break; }
    }
    free(d); return found;
}

/* descend the leading components of `path`, returning the parent dir sector
 * and the final leaf name. Returns 0 if an intermediate dir is missing. */
static uint64_t resolve_parent(FILE *f, const gfc_geom *g, const char *path, char *leaf, size_t leafsz){
    uint64_t cur=ag_data_start(g,0);            /* root */
    char buf[1024]; strncpy(buf,path,sizeof buf-1); buf[sizeof buf-1]=0;
    char *p=buf, *slash;
    while ((slash=strchr(p,'/'))!=NULL){
        *slash=0;
        if (*p){ uint64_t ns; uint8_t ty; if(!dir_find(f,g,cur,p,&ns,&ty)||ty!=OBJ_TYPE_DIR) return 0; cur=ns; }
        p=slash+1;
    }
    strncpy(leaf,p,leafsz-1); leaf[leafsz-1]=0;
    return cur;
}

/* resolve a full path to a directory sector (every component is a dir). */
static uint64_t resolve_dir(FILE *f, const gfc_geom *g, const char *path){
    uint64_t cur=ag_data_start(g,0);
    if (!path||!*path) return cur;
    char buf[1024]; strncpy(buf,path,sizeof buf-1); buf[sizeof buf-1]=0;
    char *p=strtok(buf,"/");
    while (p){ uint64_t ns; uint8_t ty; if(!dir_find(f,g,cur,p,&ns,&ty)||ty!=OBJ_TYPE_DIR) return 0; cur=ns; p=strtok(NULL,"/"); }
    return cur;
}

/* append an entry to the directory at dir_sec; -1 if the (single-cluster) dir is full */
static int dir_add_entry(FILE *f, const gfc_geom *g, uint64_t dir_sec, const char *name,
                         uint8_t type, uint32_t load, uint32_t exec, uint64_t length, uint64_t start_sec){
    uint32_t cb=1u<<g->log2_bpmb; uint8_t *d=calloc(1,cb);
    read_run(f,g,dir_sec,1,d);
    uint32_t n=get_u32(d+OBJ_HDR_BYTES);
    uint64_t capb=get_u64(d+OBJ_ClusterCount)*cb;
    if (OBJ_HDR_BYTES+4+(uint64_t)(n+1)*DIRENT_BYTES > capb){ free(d); return -1; }
    uint8_t *de=d+OBJ_HDR_BYTES+4+(size_t)n*DIRENT_BYTES;
    memset(de,0,DIRENT_BYTES); memset(de+DE_Name,' ',DE_NameLen);
    { size_t l=strlen(name); if(l>DE_NameLen)l=DE_NameLen; memcpy(de+DE_Name,name,l); }
    de[DE_Type]=type; de[DE_Attrs]=0x03;
    put_u32(de+DE_Load,load); put_u32(de+DE_Exec,exec);
    put_u64(de+DE_Length,length); put_u64(de+DE_StartSector,start_sec);
    put_u32(d+OBJ_HDR_BYTES,n+1);
    put_u64(d+OBJ_Length,4+(uint64_t)(n+1)*DIRENT_BYTES);
    put_u32(d+OBJ_HdrCheck,sum_words(d,OBJ_HdrCheck));
    write_run(f,g,dir_sec,1,d); free(d); return 0;
}

/* ====================================================================== */
static int cmd_format(int argc, char **argv)
{
    const char *path=NULL, *name="GDisc";
    uint64_t total=256ull*1024*1024;
    uint32_t sector=4096, cluster=0, agsize_bytes=64u*1024*1024;
    int i, lazy=0;
    for (i=0;i<argc;i++){
        if (!strcmp(argv[i],"--size") && i+1<argc){ if(parse_size(argv[++i],&total)){fprintf(stderr,"bad --size\n");return 2;} }
        else if(!strcmp(argv[i],"--sector")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} sector=(uint32_t)v; }
        else if(!strcmp(argv[i],"--ag-size")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} agsize_bytes=(uint32_t)v; }
        else if(!strcmp(argv[i],"--bpmb")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} cluster=(uint32_t)v; }
        else if(!strcmp(argv[i],"--name")&&i+1<argc){ name=argv[++i]; }
        else if(!strcmp(argv[i],"--lazy")){ lazy=1; }
        else if(argv[i][0]!='-' && !path){ path=argv[i]; }
        else { fprintf(stderr,"unknown arg: %s\n",argv[i]); return 2; }
    }
    if (!path){ fprintf(stderr,"usage: gfctool format <image> [opts]\n"); return 2; }
    if (!cluster) cluster=sector;

    gfc_geom g; char err[128];
    uint32_t agsize_sectors=agsize_bytes/sector;
    if (agsize_bytes % sector){ fprintf(stderr,"ag-size must be a multiple of sector size\n"); return 2; }
    if (compute_geom(total,sector,cluster,agsize_sectors,&g,err,sizeof err)){
        fprintf(stderr,"geometry error: %s\n",err); return 2; }

    /* auto-enable lazy AG init when there are too many AGs to write up front */
    if (g.agcount > 65536) lazy=1;
    uint64_t usable=total_usable_clusters(&g);

    FILE *f=fopen(path,"wb+");
    if (!f){ perror("fopen"); return 1; }

    uint8_t *buf=calloc(1,sector), *map=NULL;
    uint32_t root_local=ag_first_data_cluster(&g,0);

    /* superblocks (primary @0, secondary @1) */
    build_superblock(buf,&g,total,name,root_local,usable,lazy);
    if (write_sector(f,&g,0,buf)||write_sector(f,&g,1,buf)){ perror("write sb"); goto fail; }

    /* per-AG header + map: all AGs eagerly, or just AG0 when lazy (rest on first use) */
    map=calloc(g.map_zones,sector);
    { uint64_t aglim = lazy ? 1 : g.agcount;
      for (uint64_t i=0;i<aglim;i++){
        uint64_t cfree,ctot;
        memset(map,0,(size_t)g.map_zones*sector);
        ag_reserved_bits(map,&g,i,(i==0),&cfree,&ctot);
        map_finalise_checks(map,&g);
        build_ag_header(buf,&g,i,cfree,ctot);
        if (write_sector(f,&g,ag_header_sector(&g,i),buf)){ perror("write agh"); goto fail; }
        for (uint16_t z=0; z<g.map_zones; z++)
            if (write_sector(f,&g,ag_map_start(&g,i)+z,map+(uint32_t)z*sector)){ perror("write map"); goto fail; }
      }
    }

    /* root directory object in AG0 first data cluster */
    {
        uint8_t *cl=calloc(1,cluster);
        uint64_t root_sec = ag_data_start(&g,0);
        build_root_object(cl,cluster,root_local,root_sec);
        for (uint32_t s=0;s<(1u<<g.log2_secs_per_clu);s++)
            if (write_sector(f,&g,root_sec+s, cl+(uint32_t)s*sector)){ perror("write root"); free(cl); goto fail; }
        free(cl);
    }

    /* eager mode materialises the full image; lazy leaves it sparse/short */
    if (!lazy && gfc_fseek(f, g.total_sectors*sector - 1, SEEK_SET)==0){ uint8_t z=0; fwrite(&z,1,1,f); }

    fclose(f); free(buf); free(map);
    printf("Formatted %s: %llu AGs, sector %u, cluster %u, %u map-zone(s)/AG%s; usable %llu clusters\n",
           path,(unsigned long long)g.agcount,sector,cluster,g.map_zones,
           lazy?" [lazy]":"",(unsigned long long)usable);
    return 0;
fail:
    if(f) fclose(f);
    free(buf); free(map);
    return 1;
}

/* read+validate a superblock copy at `lba` into sb; returns 0 if valid. */
static int try_superblock(FILE *f, uint8_t *sb, uint64_t lba)
{
    /* sector size lives at byte 0 of the primary (offset 0) - the one position-independent
     * field; both copies are ss-aligned at lba*ss. */
    uint8_t head[512];
    if (gfc_fseek(f,0,SEEK_SET)||fread(head,1,512,f)!=512) return -1;
    uint32_t ss=1u<<head[SB_Log2SectorSize];
    if (ss<512||ss>4096||!is_pow2_u64(ss)) return -1;
    if (gfc_fseek(f,lba*ss,SEEK_SET)||fread(sb,1,ss,f)!=ss) return -1;
    if (get_u32(sb+SB_GFC_MAGIC)!=GFC_SB_MAGIC) return -1;
    if (get_u32(sb+ss-4)!=gfc_struct_check(sb,ss)) return -1;   /* checksum */
    return 0;
}

/* ---- read SB and rebuild geometry for check/info ---- */
static int load_geom(FILE *f, gfc_geom *g, uint8_t *sb, uint32_t sector_guess, char *err, size_t errsz)
{
    (void)sector_guess;
    /* primary @ sector 0; fall back to the secondary copy @ sector 1 (recovery) */
    if (try_superblock(f,sb,0)!=0){
        if (try_superblock(f,sb,1)!=0){ snprintf(err,errsz,"no valid superblock (primary and secondary both bad)"); return -1; }
        fprintf(stderr,"warning: primary superblock bad - recovered from secondary copy\n");
    }
    uint32_t ss = 1u<<sb[SB_Log2SectorSize];
    uint64_t total_bytes = get_u64(sb+SB_TotalSectors) * ss;
    uint32_t cluster = 1u << (sb[SB_Log2SectorSize]+sb[SB_Log2SecsPerClu]);
    uint32_t agsize_sectors = 1u<<sb[SB_Log2AGSize];
    if (compute_geom(total_bytes,ss,cluster,agsize_sectors,g,err,errsz)) return -1;
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool info <image>\n"); return 2; }
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    char name[11]; memcpy(name,sb+SB_DiscName,10); name[10]=0;
    printf("G-format image: %s\n", argv[0]);
    printf("  disc name        : %s\n", name);
    printf("  format version   : %u.%u\n", get_u16(sb+SB_FmtMajor), get_u16(sb+SB_FmtMinor));
    printf("  sector size      : %u bytes\n", g.sector_size);
    printf("  cluster (bpmb)   : %u bytes (%u sectors)\n", 1u<<g.log2_bpmb, 1u<<g.log2_secs_per_clu);
    printf("  total sectors    : %llu (%.2f MiB)\n",(unsigned long long)g.total_sectors,
           (double)g.total_sectors*g.sector_size/1048576.0);
    printf("  allocation groups: %llu x %u sectors\n",(unsigned long long)g.agcount,g.agsize_sectors);
    printf("  clusters per AG  : %u\n", g.clusters_per_ag);
    printf("  map zones per AG : %u\n", g.map_zones);
    printf("  object id        : %u AG bits + %u local bits\n", g.objid_ag_bits, GFC_OBJID_LOCALBITS);
    printf("  root obj id      : 0x%llx\n",(unsigned long long)get_u64(sb+SB_RootObjId));
    fclose(f); return 0;
}

static int cmd_free(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool free <image>\n"); return 2; }
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint64_t tot=get_u64(sb+SB_TotalClusters), fre=get_u64(sb+SB_FreeClusters), used=tot-fre;
    double cb=(double)(1u<<g.log2_bpmb);
    printf("Free space for %s:\n", argv[0]);
    printf("  cluster size     : %.0f bytes\n", cb);
    printf("  usable total     : %llu clusters (%.2f GiB)\n",(unsigned long long)tot,(double)tot*cb/1073741824.0);
    printf("  used             : %llu clusters (%.2f GiB)\n",(unsigned long long)used,(double)used*cb/1073741824.0);
    printf("  free             : %llu clusters (%.2f GiB, %.1f%%)\n",
           (unsigned long long)fre,(double)fre*cb/1073741824.0, tot?100.0*(double)fre/(double)tot:0.0);
    printf("  allocation groups: %llu%s\n",(unsigned long long)g.agcount,
           (get_u32(sb+SB_FeatureFlags)&FEATURE_LAZY_AG)?" [lazy AG init]":"");
    fclose(f); return 0;
}

/* growable set of (AG, local cluster) pairs collected while walking the tree */
typedef struct { uint64_t *ag; uint32_t *lc; uint64_t cap, M; } cluset;
static void cluset_add(cluset *s, const gfc_geom *g, uint64_t sec){
    if (s->M==s->cap){ s->cap*=2; s->ag=realloc(s->ag,s->cap*sizeof*s->ag); s->lc=realloc(s->lc,s->cap*sizeof*s->lc); }
    s->ag[s->M]=sec_to_ag(g,sec); s->lc[s->M]=sec_to_localclu(g,sec); s->M++;
}
#define CFAIL(...) do{ fprintf(stderr,"FAIL: "); fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); err++; }while(0)
/* validate the directory at dir_sec and collect every cluster it (recursively) owns */
static int collect_tree(FILE *f, const gfc_geom *g, uint64_t dir_sec, cluset *s, int depth)
{
    int err=0;
    if (depth>64){ fprintf(stderr,"FAIL: directory nesting too deep\n"); return 1; }
    uint32_t cb=1u<<g->log2_bpmb; uint8_t *d=calloc(1,cb);
    if (read_run(f,g,dir_sec,1,d)){ CFAIL("cannot read directory @ sector %llu",(unsigned long long)dir_sec); free(d); return err; }
    if (get_u32(d+OBJ_Magic)!=GFC_OBJ_MAGIC) CFAIL("dir @ %llu bad magic",(unsigned long long)dir_sec);
    if (d[OBJ_Type]!=OBJ_TYPE_DIR) CFAIL("object @ %llu is not a directory",(unsigned long long)dir_sec);
    if (get_u32(d+OBJ_HdrCheck)!=sum_words(d,OBJ_HdrCheck)) CFAIL("dir @ %llu header checksum",(unsigned long long)dir_sec);
    uint32_t n=get_u32(d+OBJ_HDR_BYTES);
    for (uint32_t e=0;e<n;e++){
        const uint8_t *de=d+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        uint64_t st=get_u64(de+DE_StartSector); uint8_t ty=de[DE_Type];
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        if (st>=g->total_sectors){ CFAIL("'%s' entry out of range",nm); continue; }
        if (ty==OBJ_TYPE_DIR){
            cluset_add(s,g,st);                       /* subdirectory cluster */
            err += collect_tree(f,g,st,s,depth+1);    /* recurse */
        } else {
            uint8_t *hc=calloc(1,cb); read_run(f,g,st,1,hc);
            if (get_u32(hc+OBJ_Magic)!=GFC_OBJ_MAGIC) CFAIL("'%s' object bad magic",nm);
            else if (get_u32(hc+OBJ_HdrCheck)!=sum_words(hc,OBJ_HdrCheck)) CFAIL("'%s' object header checksum",nm);
            else {
                cluset_add(s,g,st);                   /* file header cluster */
                uint16_t nx=get_u16(hc+OBJ_ExtentCount); uint64_t dc=0;
                for (uint16_t x=0;x<nx;x++){
                    uint64_t es=get_u64(hc+OBJ_HDR_BYTES+(size_t)x*EXTENT_BYTES+EXT_StartSector);
                    uint64_t ec=get_u64(hc+OBJ_HDR_BYTES+(size_t)x*EXTENT_BYTES+EXT_ClusterCount);
                    for (uint64_t k=0;k<ec;k++){
                        uint64_t sec=es+(k<<g->log2_secs_per_clu);
                        if (sec>=g->total_sectors){ CFAIL("'%s' extent out of range",nm); break; }
                        cluset_add(s,g,sec);
                    }
                    dc+=ec;
                }
                if (dc!=get_u64(hc+OBJ_ClusterCount)) CFAIL("'%s' DataClusters != sum of extents",nm);
            }
            free(hc);
        }
    }
    free(d); return err;
}
#undef CFAIL

#define FAIL(...) do{ fprintf(stderr,"FAIL: "); fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); nerr++; }while(0)
static int cmp_u64(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b; return (x>y)-(x<y);
}

static int cmd_check(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool check <image>\n"); return 2; }
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    int nerr=0;

    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"FAIL: %s\n",err); fclose(f); return 1; }

    /* 1. validate BOTH superblock copies independently (primary @0, secondary @1) */
    { uint32_t ss=g.sector_size; uint8_t *p=malloc(ss), *s2=malloc(ss);
      int pf=try_superblock(f,p,0), sf=try_superblock(f,s2,1);
      if (pf) FAIL("primary superblock invalid (magic/checksum)");
      if (sf) FAIL("secondary superblock invalid (magic/checksum)");
      if (!pf && !sf && memcmp(p,s2,ss)) FAIL("primary and secondary superblocks differ");
      free(p); free(s2); }

    int lazy = (get_u32(sb+SB_FeatureFlags)&FEATURE_LAZY_AG)!=0;

    /* 2. geometry vs image length (eager only; lazy images are sparse/short) */
    if (!lazy && gfc_fseek(f,0,SEEK_END)==0){
        uint64_t len=(uint64_t)gfc_ftell(f);
        if (len != g.total_sectors*g.sector_size) FAIL("image length %llu != TotalSectors*sector %llu",
            (unsigned long long)len,(unsigned long long)(g.total_sectors*g.sector_size));
    }
    if (get_u64(sb+SB_AGCount)!=g.agcount) FAIL("AGCount field %llu != computed %llu",
        (unsigned long long)get_u64(sb+SB_AGCount),(unsigned long long)g.agcount);

    /* 3. recursively walk the directory tree from root, validating objects and
     *    collecting every cluster they own (header clusters, file extents, subdirs). */
    cluset S; S.cap=64; S.M=0; S.ag=malloc(S.cap*sizeof*S.ag); S.lc=malloc(S.cap*sizeof*S.lc);
    nerr += collect_tree(f,&g,ag_data_start(&g,0),&S,0);

    /* 3b. global free-space counter consistency (free = usable - objects - root) */
    uint64_t usable=total_usable_clusters(&g);
    if (get_u64(sb+SB_TotalClusters)!=usable) FAIL("TotalClusters %llu != computed usable %llu",
        (unsigned long long)get_u64(sb+SB_TotalClusters),(unsigned long long)usable);
    if (get_u64(sb+SB_FreeClusters)+S.M+1 != usable) FAIL("FreeClusters %llu inconsistent (usable %llu, allocated %llu)",
        (unsigned long long)get_u64(sb+SB_FreeClusters),(unsigned long long)usable,(unsigned long long)(S.M+1));

    /* 4. validate AG0 + every AG that owns an object cluster (O(used AGs); scales to 16 EB) */
    uint64_t *uag=malloc((S.M+1)*sizeof*uag), U=0; uag[U++]=0;
    for (uint64_t m=0;m<S.M;m++) uag[U++]=S.ag[m];
    qsort(uag,U,sizeof*uag,cmp_u64);
    { uint64_t W=0; for(uint64_t m=0;m<U;m++) if(W==0||uag[m]!=uag[W-1]) uag[W++]=uag[m]; U=W; }

    uint8_t *h=malloc(g.sector_size), *map=malloc((size_t)g.map_zones*g.sector_size);
    uint8_t *exp=malloc((size_t)g.map_zones*g.sector_size);
    for (uint64_t ui=0; ui<U && nerr<50; ui++){
        uint64_t i=uag[ui];
        if (read_sector(f,&g,ag_header_sector(&g,i),h)){ FAIL("read AG %llu header",(unsigned long long)i); continue; }
        if (get_u32(h+AGH_Magic)!=GFC_AGH_MAGIC) FAIL("AG %llu bad magic",(unsigned long long)i);
        if (get_u64(h+AGH_AGNumber)!=i) FAIL("AG %llu number field wrong",(unsigned long long)i);
        if (get_u64(h+AGH_BaseSector)!=ag_base(&g,i)) FAIL("AG %llu base sector",(unsigned long long)i);
        if (get_u32(h+g.sector_size-4)!=gfc_struct_check(h,g.sector_size)) FAIL("AG %llu header checksum",(unsigned long long)i);

        uint8_t cross=0;
        for (uint16_t z=0; z<g.map_zones; z++){
            if (read_sector(f,&g,ag_map_start(&g,i)+z, map+(uint32_t)z*g.sector_size)){ FAIL("AG %llu read map zone %u",(unsigned long long)i,z); continue; }
            uint8_t *zs=map+(uint32_t)z*g.sector_size;
            uint8_t want=gfc_zone_check(zs,g.sector_size);
            if (zs[0]!=want) FAIL("AG %llu zone %u ZoneCheck (got %02x want %02x)",(unsigned long long)i,z,zs[0],want);
            cross ^= zs[1];
        }
        if (cross!=0xFF) FAIL("AG %llu CrossCheck EOR=%02x (want ff)",(unsigned long long)i,cross);

        uint64_t cfree,ctot,lg, real=ag_real_clusters(&g,i);
        memset(exp,0,(size_t)g.map_zones*g.sector_size);
        ag_reserved_bits(exp,&g,i,1,&cfree,&ctot);            /* reserved (+root in AG0) */
        for (uint64_t m=0;m<S.M;m++) if (S.ag[m]==i) map_set(exp,&g,S.lc[m]);
        map_scan_free(exp,&g,real,&cfree,&lg);                /* free after objects */
        for (uint32_t c=0;c<g.clusters_per_ag;c++)
            if (map_get(map,&g,c)!=map_get(exp,&g,c)){ FAIL("AG %llu cluster %u allocation bit mismatch (map vs objects)",(unsigned long long)i,c); break; }
        if (get_u64(h+AGH_ClustersFree)!=cfree) FAIL("AG %llu ClustersFree %llu != expected %llu",
            (unsigned long long)i,(unsigned long long)get_u64(h+AGH_ClustersFree),(unsigned long long)cfree);
        if (get_u64(h+AGH_ClustersTotal)!=ctot) FAIL("AG %llu ClustersTotal mismatch",(unsigned long long)i);
    }
    free(uag); free(h); free(map); free(exp); free(S.ag); free(S.lc);

    fclose(f);
    if (nerr){ printf("CHECK FAILED: %d error(s)\n",nerr); return 1; }
    printf("CHECK OK: %llu AGs%s, %llu sectors, all structures consistent\n",
           (unsigned long long)g.agcount, lazy?" [lazy]":"", (unsigned long long)g.total_sectors);
    return 0;
}

/* ====================================================================== */
/* journaling: sidecar ‹image›.gfcjrnl, mirrors the FileCore hook semantics */

static void jrec_common(uint8_t type){
    uint8_t h[JREC_HDR_BYTES]; memset(h,0,sizeof h);
    put_u32(h+0,JREC_MAGIC); put_u64(h+4,J.seq); put_u64(h+12,J.txn);
    h[20]=type; h[21]=J.drive;
    fwrite(h,JREC_HDR_BYTES,1,J.jf);
    J.seq++;
}
static void jcapture_before(FILE *f, const void *gv, uint64_t sec){
    const gfc_geom *g=gv;
    uint8_t *bi=malloc(g->sector_size);
    if (read_sector(f,g,sec,bi)) memset(bi,0,g->sector_size);
    jrec_common(JREC_WRITE);
    { uint8_t t[12]; put_u64(t,sec); put_u32(t+8,1); fwrite(t,12,1,J.jf); }
    fwrite(bi,g->sector_size,1,J.jf);
    free(bi);
}
static char *jpath(const char *imgpath){
    char *p=malloc(strlen(imgpath)+10); sprintf(p,"%s.gfcjrnl",imgpath); return p;
}
static int jopen(const char *imgpath, FILE *imgf, const gfc_geom *g){
    char *jp=jpath(imgpath);
    FILE *jf=fopen(jp,"rb+"); free(jp);
    uint64_t next_seq=1;
    if (jf){
        uint8_t hd[32];
        if (fread(hd,1,32,jf)!=32 || get_u32(hd)!=JRNL_MAGIC ||
            get_u32(hd+8)!=g->sector_size || get_u64(hd+12)!=g->total_sectors){ fclose(jf); return -1; }
        next_seq=get_u64(hd+20);
        gfc_fseek(jf,0,SEEK_END);
    } else {
        char *jp2=jpath(imgpath); jf=fopen(jp2,"wb+"); free(jp2);
        if (!jf) return -1;
        uint8_t hd[32]; memset(hd,0,32);
        put_u32(hd,JRNL_MAGIC); put_u16(hd+4,1);
        put_u32(hd+8,g->sector_size); put_u64(hd+12,g->total_sectors); put_u64(hd+20,next_seq);
        fwrite(hd,32,1,jf);
    }
    J.jf=jf; J.imgf=imgf; J.g=g; J.seq=next_seq; J.drive=0; J.active=0;
    return 0;
}
static void jbegin(const char *tag){
    J.txn=J.seq; jrec_common(JREC_BEGIN);
    uint16_t n=(uint16_t)strlen(tag); uint8_t t[2]; put_u16(t,n);
    fwrite(t,2,1,J.jf); fwrite(tag,n,1,J.jf);
    J.active=1;
}
static void jcommit(void){ J.active=0; jrec_common(JREC_COMMIT); }
static void jclose(void){
    if (!J.jf) return;
    uint8_t hd[32];
    gfc_fseek(J.jf,0,SEEK_SET); if(fread(hd,1,32,J.jf)!=32){}
    put_u64(hd+20,J.seq);
    gfc_fseek(J.jf,0,SEEK_SET); fwrite(hd,32,1,J.jf);
    fclose(J.jf); memset(&J,0,sizeof J);
}

static int cmd_journal(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool journal <image>\n"); return 2; }
    char *jp=jpath(argv[0]); FILE *jf=fopen(jp,"rb"); free(jp);
    if (!jf){ printf("no journal for %s\n",argv[0]); return 0; }
    uint8_t hd[32];
    if (fread(hd,1,32,jf)!=32 || get_u32(hd)!=JRNL_MAGIC){ fprintf(stderr,"bad journal header\n"); fclose(jf); return 1; }
    uint32_t ss=get_u32(hd+8);
    printf("journal for %s: sector %u, next_seq %llu\n",argv[0],ss,(unsigned long long)get_u64(hd+20));
    for(;;){
        uint8_t rh[JREC_HDR_BYTES];
        if (fread(rh,1,JREC_HDR_BYTES,jf)!=JREC_HDR_BYTES) break;
        if (get_u32(rh)!=JREC_MAGIC){ fprintf(stderr,"corrupt record\n"); break; }
        uint64_t seq=get_u64(rh+4), txn=get_u64(rh+12); uint8_t type=rh[20];
        if (type==JREC_BEGIN){
            uint8_t t[2]; if(fread(t,1,2,jf)!=2)break; uint16_t n=get_u16(t);
            char *tag=malloc((size_t)n+1); if(fread(tag,1,n,jf)!=n){free(tag);break;} tag[n]=0;
            printf("  txn %llu  BEGIN  \"%s\"\n",(unsigned long long)txn,tag); free(tag);
        } else if (type==JREC_COMMIT){
            printf("  txn %llu  COMMIT (seq %llu)\n",(unsigned long long)txn,(unsigned long long)seq);
        } else if (type==JREC_WRITE){
            uint8_t t[12]; if(fread(t,1,12,jf)!=12)break;
            uint64_t sec=get_u64(t); uint32_t ns=get_u32(t+8);
            gfc_fseek(jf,(uint64_t)ns*ss,SEEK_CUR);
            printf("    seq %llu  WRITE %u sector(s) @ %llu (before-image saved)\n",
                   (unsigned long long)seq,ns,(unsigned long long)sec);
        } else break;
    }
    fclose(jf); return 0;
}

static int cmd_rewind(int argc, char **argv)
{
    const char *imgp=NULL; int have_to=0; uint64_t to_txn=0;
    for (int i=0;i<argc;i++){
        if (!strcmp(argv[i],"--to") && i+1<argc){ have_to=1; to_txn=strtoull(argv[++i],NULL,10); }
        else if (argv[i][0]!='-' && !imgp) imgp=argv[i];
        else { fprintf(stderr,"bad arg: %s\n",argv[i]); return 2; }
    }
    if (!imgp){ fprintf(stderr,"usage: gfctool rewind <image> [--to TXN]\n"); return 2; }

    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t ss=g.sector_size;

    char *jp=jpath(imgp); FILE *jf=fopen(jp,"rb"); free(jp);
    if (!jf){ fprintf(stderr,"no journal to rewind\n"); fclose(f); return 1; }
    uint8_t hd[32];
    if (fread(hd,1,32,jf)!=32 || get_u32(hd)!=JRNL_MAGIC){ fprintf(stderr,"bad journal\n"); fclose(jf); fclose(f); return 1; }

    /* parse forward: collect begins and write records (with before-image offsets) */
    struct { uint64_t txn, off; } *txns=NULL; int ntxn=0, captxn=0;
    struct { uint64_t txn, sec, bioff; uint32_t ns; } *wr=NULL; int nwr=0, capwr=0;
    uint64_t maxtxn=0;
    for(;;){
        uint64_t roff=(uint64_t)gfc_ftell(jf);
        uint8_t rh[JREC_HDR_BYTES];
        if (fread(rh,1,JREC_HDR_BYTES,jf)!=JREC_HDR_BYTES) break;
        if (get_u32(rh)!=JREC_MAGIC) break;
        uint64_t txn=get_u64(rh+12); uint8_t type=rh[20];
        if (type==JREC_BEGIN){
            uint8_t t[2]; if(fread(t,1,2,jf)!=2)break; uint16_t n=get_u16(t); gfc_fseek(jf,n,SEEK_CUR);
            if(ntxn==captxn){ captxn=captxn?captxn*2:16; txns=realloc(txns,captxn*sizeof *txns); }
            txns[ntxn].txn=txn; txns[ntxn].off=roff; ntxn++;
            if (txn>maxtxn) maxtxn=txn;
        } else if (type==JREC_COMMIT){
            /* nothing */
        } else if (type==JREC_WRITE){
            uint8_t t[12]; if(fread(t,1,12,jf)!=12)break;
            uint64_t sec=get_u64(t); uint32_t ns=get_u32(t+8);
            uint64_t bioff=(uint64_t)gfc_ftell(jf);
            gfc_fseek(jf,(uint64_t)ns*ss,SEEK_CUR);
            if(nwr==capwr){ capwr=capwr?capwr*2:64; wr=realloc(wr,capwr*sizeof *wr); }
            wr[nwr].txn=txn; wr[nwr].sec=sec; wr[nwr].ns=ns; wr[nwr].bioff=bioff; nwr++;
        } else break;
    }
    if (ntxn==0){ fprintf(stderr,"journal empty, nothing to rewind\n"); fclose(jf); fclose(f); free(txns); free(wr); return 1; }

    /* which txns are undone? */
    #define UNDONE(T) (have_to ? (T)>to_txn : (T)==maxtxn)
    uint64_t earliest=0, trunc_off=0; int any=0;
    for (int i=0;i<ntxn;i++) if (UNDONE(txns[i].txn)){
        if (!any || txns[i].txn<earliest){ earliest=txns[i].txn; trunc_off=txns[i].off; }
        any=1;
    }
    if (!any){ printf("nothing to rewind (target txn %llu)\n",(unsigned long long)to_txn); fclose(jf); fclose(f); free(txns); free(wr); return 0; }

    /* apply before-images in reverse sequence order for undone txns */
    int undone_w=0, undone_t=0;
    uint8_t *bi=malloc(ss);
    for (int i=nwr-1;i>=0;i--) if (UNDONE(wr[i].txn)){
        for (uint32_t k=0;k<wr[i].ns;k++){
            gfc_fseek(jf, wr[i].bioff + (uint64_t)k*ss, SEEK_SET);
            if (fread(bi,1,ss,jf)!=ss){ fprintf(stderr,"short before-image read\n"); break; }
            write_sector(f,&g,wr[i].sec+k,bi);     /* J inactive: not re-journalled */
        }
        undone_w++;
    }
    for (int i=0;i<ntxn;i++) if (UNDONE(txns[i].txn)) undone_t++;
    free(bi);

    /* truncate journal: keep prefix [0,trunc_off), reset next_seq to earliest */
    uint8_t *prefix=malloc(trunc_off);
    gfc_fseek(jf,0,SEEK_SET); if(fread(prefix,1,trunc_off,jf)!=trunc_off){}
    put_u64(prefix+20,earliest);
    fclose(jf);
    { char *jp2=jpath(imgp); FILE *jw=fopen(jp2,"wb"); free(jp2);
      if (jw){ fwrite(prefix,1,trunc_off,jw); fclose(jw); } }
    free(prefix); free(txns); free(wr); fclose(f);

    printf("rewound %d transaction(s), restored %d write record(s); journal now ends before txn %llu\n",
           undone_t, undone_w, (unsigned long long)earliest);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool ls <image> [path]\n"); return 2; }
    const char *path = argc>1 ? argv[1] : "";
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint64_t dir_sec=resolve_dir(f,&g,path);
    if (!dir_sec){ fprintf(stderr,"directory not found: %s\n",path); fclose(f); return 1; }
    uint8_t *d=calloc(1,1u<<g.log2_bpmb);
    if (read_run(f,&g,dir_sec,1,d)){ fprintf(stderr,"cannot read directory\n"); free(d); fclose(f); return 1; }
    uint32_t nent=get_u32(d+OBJ_HDR_BYTES);
    printf("%s (%u object%s)\n", (path&&*path)?path:"$", nent, nent==1?"":"s");
    for (uint32_t e=0;e<nent;e++){
        const uint8_t *de=d+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        printf("  %-12s  %-4s  %10llu bytes  load=%08x exec=%08x\n",
               nm, de[DE_Type]==OBJ_TYPE_DIR?"dir":"file",
               (unsigned long long)get_u64(de+DE_Length), get_u32(de+DE_Load), get_u32(de+DE_Exec));
    }
    free(d); fclose(f); return 0;
}

static int cmd_mkfile(int argc, char **argv)
{
    if (argc<3){ fprintf(stderr,"usage: gfctool mkfile <image> <name> <srcfile>\n"); return 2; }
    const char *imgp=argv[0], *name=argv[1], *srcp=argv[2];
    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;
    int max_ext=(clu_bytes-OBJ_HDR_BYTES)/EXTENT_BYTES;

    char leaf[64]; uint64_t parent=resolve_parent(f,&g,name,leaf,sizeof leaf);
    if(!parent){ fprintf(stderr,"path not found: %s\n",name); fclose(f); return 1; }

    FILE *s=fopen(srcp,"rb"); if(!s){ perror("fopen src"); fclose(f); return 1; }
    gfc_fseek(s,0,SEEK_END); uint64_t fsize=(uint64_t)gfc_ftell(s); gfc_fseek(s,0,SEEK_SET);
    uint64_t data_clu=ceil_div(fsize, clu_bytes);          /* data clusters (header is separate) */

    /* precheck free space (header + data) before mutating anything */
    if (total_free_clusters(f,&g) < data_clu+1){
        fprintf(stderr,"not enough free space (%llu clusters needed)\n",(unsigned long long)(data_clu+1));
        fclose(s); fclose(f); return 1; }
    /* precheck parent directory has room (avoid orphaning clusters) */
    { uint8_t *pd=calloc(1,clu_bytes); read_run(f,&g,parent,1,pd);
      uint32_t pn=get_u32(pd+OBJ_HDR_BYTES); uint64_t cap=get_u64(pd+OBJ_ClusterCount)*clu_bytes; free(pd);
      if (OBJ_HDR_BYTES+4+(uint64_t)(pn+1)*DIRENT_BYTES > cap){ fprintf(stderr,"directory full\n"); fclose(s); fclose(f); return 1; } }

    char tag[80]; snprintf(tag,sizeof tag,"mkfile %s",name);
    int jrn = (jopen(imgp,f,&g)==0);
    if (jrn) jbegin(tag); else fprintf(stderr,"warning: journaling disabled\n");

    /* allocate: 1 header cluster, then the data clusters (possibly fragmented / cross-AG) */
    extent_t hext, *dext=malloc((size_t)(max_ext+1)*sizeof *dext);
    int nh, nd;
    if (alloc_clusters(f,&g,1,&hext,&nh,1) || alloc_clusters(f,&g,data_clu,dext,&nd,max_ext)){
        fprintf(stderr,"allocation failed (too fragmented for %d extents?)\n",max_ext);
        free(dext); fclose(s); fclose(f); return 1; }
    uint64_t hdr_sec=hext.start_sector;

    /* build + write header cluster (object record + extent table) */
    uint8_t *hc=calloc(1,clu_bytes);
    uint64_t objid=((uint64_t)sec_to_ag(&g,hdr_sec)<<GFC_OBJID_LOCALBITS)|sec_to_localclu(&g,hdr_sec);
    build_obj_header(hc,objid,OBJ_TYPE_FILE,0x03,(uint16_t)nd,0xFFFFFD00u,0,fsize,hdr_sec,data_clu,leaf);
    for (int e=0;e<nd;e++){
        put_u64(hc+OBJ_HDR_BYTES+e*EXTENT_BYTES+EXT_StartSector,dext[e].start_sector);
        put_u64(hc+OBJ_HDR_BYTES+e*EXTENT_BYTES+EXT_ClusterCount,dext[e].cluster_count);
    }
    put_u32(hc+OBJ_HdrCheck,sum_words(hc,OBJ_HdrCheck));
    write_run(f,&g,hdr_sec,1,hc);
    free(hc);

    /* stream file data into the allocated extents */
    { uint8_t *cl=calloc(1,clu_bytes); uint64_t left=fsize;
      for (int e=0;e<nd && left>0;e++)
        for (uint64_t k=0;k<dext[e].cluster_count && left>0;k++){
            uint64_t n = left<clu_bytes ? left : clu_bytes;
            memset(cl,0,clu_bytes);
            if (fread(cl,1,n,s)!=n){ fprintf(stderr,"short read of src\n"); }
            write_run(f,&g,dext[e].start_sector+(k<<g.log2_secs_per_clu),1,cl);
            left-=n;
        }
      free(cl); }
    free(dext); fclose(s);

    /* append directory entry in the parent dir (points at the header cluster) */
    dir_add_entry(f,&g,parent,leaf,OBJ_TYPE_FILE,0xFFFFFD00u,0,fsize,hdr_sec);
    sb_adjust_free(f,&g,-(int64_t)(1+data_clu));     /* header + data clusters consumed */
    if (jrn){ jcommit(); jclose(); }
    fclose(f);

    printf("added '%s' (%llu bytes, %llu data cluster(s) in %d extent(s)) header @ sector %llu\n",
           name,(unsigned long long)fsize,(unsigned long long)data_clu,nd,(unsigned long long)hdr_sec);
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    if (argc<3){ fprintf(stderr,"usage: gfctool read <image> <name> <outfile>\n"); return 2; }
    const char *imgp=argv[0], *name=argv[1], *outp=argv[2];
    FILE *f=fopen(imgp,"rb"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;

    /* resolve path -> parent dir -> entry */
    char leaf[64]; uint64_t parent=resolve_parent(f,&g,name,leaf,sizeof leaf);
    uint64_t hdr_sec=0; uint8_t ty=0;
    if (!parent || !dir_find(f,&g,parent,leaf,&hdr_sec,&ty)){ fprintf(stderr,"'%s' not found\n",name); fclose(f); return 1; }
    if (ty!=OBJ_TYPE_FILE){ fprintf(stderr,"'%s' is not a file\n",name); fclose(f); return 1; }

    /* header cluster -> extents -> data */
    uint8_t *hc=calloc(1,clu_bytes);
    read_run(f,&g,hdr_sec,1,hc);
    if (get_u32(hc+OBJ_Magic)!=GFC_OBJ_MAGIC){ fprintf(stderr,"bad object magic\n"); free(hc); fclose(f); return 1; }
    uint64_t length=get_u64(hc+OBJ_Length); uint16_t nx=get_u16(hc+OBJ_ExtentCount);

    FILE *o=fopen(outp,"wb"); if(!o){ perror("fopen out"); free(hc); fclose(f); return 1; }
    uint8_t *cl=calloc(1,clu_bytes); uint64_t left=length;
    for (uint16_t e=0;e<nx && left>0;e++){
        uint64_t st=get_u64(hc+OBJ_HDR_BYTES+e*EXTENT_BYTES+EXT_StartSector);
        uint64_t cc=get_u64(hc+OBJ_HDR_BYTES+e*EXTENT_BYTES+EXT_ClusterCount);
        for (uint64_t k=0;k<cc && left>0;k++){
            read_run(f,&g,st+(k<<g.log2_secs_per_clu),1,cl);
            uint64_t n=left<clu_bytes?left:clu_bytes;
            fwrite(cl,1,n,o); left-=n;
        }
    }
    free(cl); free(hc); fclose(o); fclose(f);
    printf("wrote '%s' (%llu bytes, %u extent(s)) to %s\n",name,(unsigned long long)length,nx,outp);
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    if (argc<2){ fprintf(stderr,"usage: gfctool delete <image> <name>\n"); return 2; }
    const char *imgp=argv[0], *name=argv[1];
    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;

    /* resolve path -> parent dir; find the entry index/type */
    char leaf[64]; uint64_t parent=resolve_parent(f,&g,name,leaf,sizeof leaf);
    if (!parent){ fprintf(stderr,"'%s' not found\n",name); fclose(f); return 1; }
    uint8_t *dir=calloc(1,clu_bytes);
    read_run(f,&g,parent,1,dir);
    uint32_t nent=get_u32(dir+OBJ_HDR_BYTES); uint32_t idx=nent; uint64_t hdr_sec=0; uint8_t ty=0;
    for (uint32_t e=0;e<nent;e++){
        const uint8_t *de=dir+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        if (!strcmp(nm,leaf)){ idx=e; hdr_sec=get_u64(de+DE_StartSector); ty=de[DE_Type]; break; }
    }
    if (idx==nent){ fprintf(stderr,"'%s' not found\n",name); free(dir); fclose(f); return 1; }

    /* read the object; refuse to delete a non-empty directory */
    uint8_t *hc=calloc(1,clu_bytes);
    read_run(f,&g,hdr_sec,1,hc);
    if (ty==OBJ_TYPE_DIR && get_u32(hc+OBJ_HDR_BYTES)!=0){
        fprintf(stderr,"'%s' is a non-empty directory\n",name); free(hc); free(dir); fclose(f); return 1; }

    /* gather clusters to free: header + every extent cluster */
    uint16_t nx=get_u16(hc+OBJ_ExtentCount);
    uint64_t cap=1+get_u64(hc+OBJ_ClusterCount), nf=0, *secs=malloc((cap+1)*sizeof*secs);
    secs[nf++]=hdr_sec;
    for (uint16_t e=0;e<nx;e++){
        uint64_t st=get_u64(hc+OBJ_HDR_BYTES+(size_t)e*EXTENT_BYTES+EXT_StartSector);
        uint64_t cc=get_u64(hc+OBJ_HDR_BYTES+(size_t)e*EXTENT_BYTES+EXT_ClusterCount);
        for (uint64_t k=0;k<cc;k++) secs[nf++]=st+(k<<g.log2_secs_per_clu);
    }
    free(hc);

    /* journalled transaction: free clusters + remove the parent's entry */
    char tag[80]; snprintf(tag,sizeof tag,"delete %s",name);
    int jrn=(jopen(imgp,f,&g)==0); if(jrn) jbegin(tag);
    free_clusters(f,&g,secs,(int)nf);
    sb_adjust_free(f,&g,(int64_t)nf);                 /* clusters returned to free space */
    uint8_t *base=dir+OBJ_HDR_BYTES+4;
    memmove(base+(size_t)idx*DIRENT_BYTES, base+(size_t)(idx+1)*DIRENT_BYTES,
            (size_t)(nent-idx-1)*DIRENT_BYTES);
    put_u32(dir+OBJ_HDR_BYTES,nent-1);
    put_u64(dir+OBJ_Length,4+(uint64_t)(nent-1)*DIRENT_BYTES);
    put_u32(dir+OBJ_HdrCheck,sum_words(dir,OBJ_HdrCheck));
    write_run(f,&g,parent,1,dir);
    if (jrn){ jcommit(); jclose(); }
    free(secs); free(dir); fclose(f);

    printf("deleted '%s' (freed %llu cluster(s))\n",name,(unsigned long long)nf);
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc<2){ fprintf(stderr,"usage: gfctool mkdir <image> <path>\n"); return 2; }
    const char *imgp=argv[0], *path=argv[1];
    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;

    char leaf[64]; uint64_t parent=resolve_parent(f,&g,path,leaf,sizeof leaf);
    if (!parent){ fprintf(stderr,"path not found: %s\n",path); fclose(f); return 1; }
    if (dir_find(f,&g,parent,leaf,NULL,NULL)){ fprintf(stderr,"'%s' already exists\n",path); fclose(f); return 1; }
    if (total_free_clusters(f,&g) < 1){ fprintf(stderr,"no free space\n"); fclose(f); return 1; }
    { uint8_t *pd=calloc(1,clu_bytes); read_run(f,&g,parent,1,pd);
      uint32_t pn=get_u32(pd+OBJ_HDR_BYTES); uint64_t cap=get_u64(pd+OBJ_ClusterCount)*clu_bytes; free(pd);
      if (OBJ_HDR_BYTES+4+(uint64_t)(pn+1)*DIRENT_BYTES > cap){ fprintf(stderr,"parent directory full\n"); fclose(f); return 1; } }

    char tag[80]; snprintf(tag,sizeof tag,"mkdir %s",path);
    int jrn=(jopen(imgp,f,&g)==0); if(jrn) jbegin(tag);
    extent_t he; int nh;
    if (alloc_clusters(f,&g,1,&he,&nh,1)){ fprintf(stderr,"allocation failed\n"); fclose(f); return 1; }
    uint8_t *dc=calloc(1,clu_bytes);
    uint64_t objid=((uint64_t)sec_to_ag(&g,he.start_sector)<<GFC_OBJID_LOCALBITS)|sec_to_localclu(&g,he.start_sector);
    build_obj_header(dc,objid,OBJ_TYPE_DIR,0x03,0,0,0,4,he.start_sector,1,leaf);
    put_u32(dc+OBJ_HDR_BYTES,0);   /* EntryCount = 0 */
    write_run(f,&g,he.start_sector,1,dc);
    free(dc);
    dir_add_entry(f,&g,parent,leaf,OBJ_TYPE_DIR,0,0,4,he.start_sector);
    sb_adjust_free(f,&g,-1);                          /* one cluster for the directory */
    if (jrn){ jcommit(); jclose(); }
    fclose(f);
    printf("created directory '%s' at sector %llu\n",path,(unsigned long long)he.start_sector);
    return 0;
}

/* remove entry `idx` from a dir cluster buffer (caller writes it back) */
static void dir_remove_idx(uint8_t *d, uint32_t idx){
    uint32_t n=get_u32(d+OBJ_HDR_BYTES);
    uint8_t *base=d+OBJ_HDR_BYTES+4;
    memmove(base+(size_t)idx*DIRENT_BYTES, base+(size_t)(idx+1)*DIRENT_BYTES,
            (size_t)(n-idx-1)*DIRENT_BYTES);
    put_u32(d+OBJ_HDR_BYTES,n-1);
    put_u64(d+OBJ_Length,4+(uint64_t)(n-1)*DIRENT_BYTES);
    put_u32(d+OBJ_HdrCheck,sum_words(d,OBJ_HdrCheck));
}

static int cmd_rename(int argc, char **argv)
{
    if (argc<3){ fprintf(stderr,"usage: gfctool rename <image> <oldpath> <newpath>\n"); return 2; }
    const char *imgp=argv[0], *oldp=argv[1], *newp=argv[2];
    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;

    /* locate source entry in its parent */
    char oleaf[64]; uint64_t op=resolve_parent(f,&g,oldp,oleaf,sizeof oleaf);
    if (!op){ fprintf(stderr,"'%s' not found\n",oldp); fclose(f); return 1; }
    uint8_t *od=calloc(1,clu_bytes); read_run(f,&g,op,1,od);
    uint32_t on=get_u32(od+OBJ_HDR_BYTES), oidx=on;
    uint64_t hdr_sec=0,len=0; uint32_t load=0,exec=0; uint8_t ty=0;
    for (uint32_t e=0;e<on;e++){
        const uint8_t *de=od+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        if(!strcmp(nm,oleaf)){ oidx=e; hdr_sec=get_u64(de+DE_StartSector); len=get_u64(de+DE_Length);
            load=get_u32(de+DE_Load); exec=get_u32(de+DE_Exec); ty=de[DE_Type]; break; }
    }
    if (oidx==on){ fprintf(stderr,"'%s' not found\n",oldp); free(od); fclose(f); return 1; }

    /* resolve destination parent + leaf */
    char nleaf[64]; uint64_t np=resolve_parent(f,&g,newp,nleaf,sizeof nleaf);
    if (!np){ fprintf(stderr,"destination path not found: %s\n",newp); free(od); fclose(f); return 1; }
    if (dir_find(f,&g,np,nleaf,NULL,NULL)){ fprintf(stderr,"'%s' already exists\n",newp); free(od); fclose(f); return 1; }
    { uint8_t *pd=calloc(1,clu_bytes); read_run(f,&g,np,1,pd);
      uint32_t pn=get_u32(pd+OBJ_HDR_BYTES); uint64_t cap=get_u64(pd+OBJ_ClusterCount)*clu_bytes; free(pd);
      if (np!=op && OBJ_HDR_BYTES+4+(uint64_t)(pn+1)*DIRENT_BYTES>cap){ fprintf(stderr,"destination directory full\n"); free(od); fclose(f); return 1; } }

    char tag[96]; snprintf(tag,sizeof tag,"rename %s -> %s",oldp,newp);
    int jrn=(jopen(imgp,f,&g)==0); if(jrn) jbegin(tag);

    /* update the object's own name */
    uint8_t *oc=calloc(1,clu_bytes); read_run(f,&g,hdr_sec,1,oc);
    memset(oc+OBJ_Name,' ',OBJ_NameLen);
    { size_t l=strlen(nleaf); if(l>OBJ_NameLen)l=OBJ_NameLen; memcpy(oc+OBJ_Name,nleaf,l); }
    put_u32(oc+OBJ_HdrCheck,sum_words(oc,OBJ_HdrCheck));
    write_run(f,&g,hdr_sec,1,oc); free(oc);

    if (np==op){
        /* same directory: rename the entry in place */
        uint8_t *de=od+OBJ_HDR_BYTES+4+(size_t)oidx*DIRENT_BYTES;
        memset(de+DE_Name,' ',DE_NameLen);
        { size_t l=strlen(nleaf); if(l>DE_NameLen)l=DE_NameLen; memcpy(de+DE_Name,nleaf,l); }
        put_u32(od+OBJ_HdrCheck,sum_words(od,OBJ_HdrCheck));
        write_run(f,&g,op,1,od);
    } else {
        /* move: remove from old parent, add to new parent */
        dir_remove_idx(od,oidx);
        write_run(f,&g,op,1,od);
        dir_add_entry(f,&g,np,nleaf,ty,load,exec,len,hdr_sec);
    }
    if (jrn){ jcommit(); jclose(); }
    free(od); fclose(f);
    printf("renamed '%s' -> '%s'\n",oldp,newp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc<2){
        fprintf(stderr,
          "gfctool - FileCore G-format reference (bounty #40)\n"
          "usage:\n"
          "  gfctool format <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR] [--lazy]\n"
          "  gfctool mkfile  <image> <name> <srcfile>\n"
          "  gfctool read    <image> <path> <outfile>\n"
          "  gfctool delete  <image> <path>\n"
          "  gfctool mkdir   <image> <path>\n"
          "  gfctool rename  <image> <oldpath> <newpath>\n"
          "  gfctool ls      <image> [path]\n"
          "  gfctool journal <image>\n"
          "  gfctool rewind  <image> [--to TXN]\n"
          "  gfctool check   <image>\n"
          "  gfctool info    <image>\n"
          "  gfctool free    <image>\n"
          "sizes accept K/M/G/T/E suffixes.\n");
        return 2;
    }
    if (!strcmp(argv[1],"format"))  return cmd_format (argc-2,argv+2);
    if (!strcmp(argv[1],"mkfile"))  return cmd_mkfile (argc-2,argv+2);
    if (!strcmp(argv[1],"read"))    return cmd_read   (argc-2,argv+2);
    if (!strcmp(argv[1],"delete"))  return cmd_delete (argc-2,argv+2);
    if (!strcmp(argv[1],"mkdir"))   return cmd_mkdir  (argc-2,argv+2);
    if (!strcmp(argv[1],"rename"))  return cmd_rename (argc-2,argv+2);
    if (!strcmp(argv[1],"ls"))      return cmd_ls     (argc-2,argv+2);
    if (!strcmp(argv[1],"journal")) return cmd_journal(argc-2,argv+2);
    if (!strcmp(argv[1],"rewind"))  return cmd_rewind (argc-2,argv+2);
    if (!strcmp(argv[1],"check"))   return cmd_check  (argc-2,argv+2);
    if (!strcmp(argv[1],"info"))    return cmd_info   (argc-2,argv+2);
    if (!strcmp(argv[1],"free"))    return cmd_free   (argc-2,argv+2);
    fprintf(stderr,"unknown command: %s\n",argv[1]);
    return 2;
}
