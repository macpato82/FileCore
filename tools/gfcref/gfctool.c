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

/* ---- map bit access ---- */
static void map_set(uint8_t *map, const gfc_geom *g, uint32_t c){
    uint32_t z=c/g->bits_per_zone, b=c%g->bits_per_zone;
    map[z*g->sector_size + ZONE_HDR_BYTES + b/8] |= (uint8_t)(1u<<(b&7));
}
static int map_get(const uint8_t *map, const gfc_geom *g, uint32_t c){
    uint32_t z=c/g->bits_per_zone, b=c%g->bits_per_zone;
    return (map[z*g->sector_size + ZONE_HDR_BYTES + b/8] >> (b&7)) & 1;
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
                             uint32_t load, uint32_t exec, uint64_t length,
                             uint64_t start_sector, uint64_t cluster_count, const char *name)
{
    memset(c,0,OBJ_HDR_BYTES);
    put_u32(c+OBJ_Magic,GFC_OBJ_MAGIC);
    put_u64(c+OBJ_ObjId,objid);
    c[OBJ_Type]=type; c[OBJ_Attrs]=attrs;
    put_u32(c+OBJ_Load,load); put_u32(c+OBJ_Exec,exec);
    put_u64(c+OBJ_Length,length);
    put_u64(c+OBJ_StartSector,start_sector);
    put_u64(c+OBJ_ClusterCount,cluster_count);
    memset(c+OBJ_Name,' ',OBJ_NameLen);
    { size_t n=strlen(name); if(n>OBJ_NameLen)n=OBJ_NameLen; memcpy(c+OBJ_Name,name,n); }
    put_u32(c+OBJ_HdrCheck, sum_words(c,OBJ_HdrCheck));
}

/* build an empty root directory object into a freshly zeroed cluster buffer */
static void build_root_object(uint8_t *c, uint32_t cluster_size, uint64_t objid, uint64_t start_sector){
    memset(c,0,cluster_size);
    build_obj_header(c,objid,OBJ_TYPE_DIR,0,0,0,4,start_sector,1,"$");
    put_u32(c+OBJ_HDR_BYTES,0);   /* EntryCount = 0 */
}

/* ---- superblock ---- */
static void build_superblock(uint8_t *sb, const gfc_geom *g, uint64_t total_bytes,
                             const char *name, uint32_t root_local)
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
    put_u32(sb+SB_FeatureFlags,0);
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
/* first-fit contiguous run of `need` free clusters in [first..real); returns cluster index or -1 */
static int64_t map_find_run(const uint8_t *map, const gfc_geom *g, uint32_t first,
                            uint64_t real_clu, uint64_t need){
    uint64_t c, run=0, start=first;
    for (c=first;c<real_clu;c++){
        if (map_get(map,g,(uint32_t)c)==0){ if(run==0) start=c; if(++run==need) return (int64_t)start; }
        else run=0;
    }
    return -1;
}

/* ====================================================================== */
static int cmd_format(int argc, char **argv)
{
    const char *path=NULL, *name="GDisc";
    uint64_t total=256ull*1024*1024;
    uint32_t sector=4096, cluster=0, agsize_bytes=64u*1024*1024;
    int i;
    for (i=0;i<argc;i++){
        if (!strcmp(argv[i],"--size") && i+1<argc){ if(parse_size(argv[++i],&total)){fprintf(stderr,"bad --size\n");return 2;} }
        else if(!strcmp(argv[i],"--sector")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} sector=(uint32_t)v; }
        else if(!strcmp(argv[i],"--ag-size")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} agsize_bytes=(uint32_t)v; }
        else if(!strcmp(argv[i],"--bpmb")&&i+1<argc){ uint64_t v; if(parse_size(argv[++i],&v)){return 2;} cluster=(uint32_t)v; }
        else if(!strcmp(argv[i],"--name")&&i+1<argc){ name=argv[++i]; }
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

    FILE *f=fopen(path,"wb+");
    if (!f){ perror("fopen"); return 1; }

    uint8_t *buf=calloc(1,sector), *map=NULL;
    uint32_t root_local=ag_first_data_cluster(&g,0);

    /* superblocks (primary @0, secondary @1) */
    build_superblock(buf,&g,total,name,root_local);
    if (write_sector(f,&g,0,buf)||write_sector(f,&g,1,buf)){ perror("write sb"); goto fail; }

    /* per-AG header + map */
    map=calloc(g.map_zones,sector);
    for (uint64_t i=0;i<g.agcount;i++){
        uint64_t cfree,ctot;
        memset(map,0,(size_t)g.map_zones*sector);
        ag_reserved_bits(map,&g,i,1,&cfree,&ctot);
        map_finalise_checks(map,&g);
        build_ag_header(buf,&g,i,cfree,ctot);
        if (write_sector(f,&g,ag_header_sector(&g,i),buf)){ perror("write agh"); goto fail; }
        for (uint16_t z=0; z<g.map_zones; z++)
            if (write_sector(f,&g,ag_map_start(&g,i)+z,map+(uint32_t)z*sector)){ perror("write map"); goto fail; }
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

    /* ensure the image is exactly TotalSectors long */
    if (gfc_fseek(f, g.total_sectors*sector - 1, SEEK_SET)==0){ uint8_t z=0; fwrite(&z,1,1,f); }

    fclose(f); free(buf); free(map);
    printf("Formatted %s: %.2f MiB, %llu AGs, sector %u, cluster %u, %u map-zone(s)/AG\n",
           path,(double)total/1048576.0,(unsigned long long)g.agcount,sector,cluster,g.map_zones);
    return 0;
fail:
    if(f) fclose(f);
    free(buf); free(map);
    return 1;
}

/* ---- read SB and rebuild geometry for check/info ---- */
static int load_geom(FILE *f, gfc_geom *g, uint8_t *sb, uint32_t sector_guess, char *err, size_t errsz)
{
    /* sector size lives at byte 0 (log2) - read first 512 to get it */
    uint8_t head[512];
    if (gfc_fseek(f,0,SEEK_SET)||fread(head,1,512,f)!=512){ snprintf(err,errsz,"cannot read header"); return -1; }
    uint32_t ss = 1u<<head[SB_Log2SectorSize];
    if (ss<512||ss>4096||!is_pow2_u64(ss)){ snprintf(err,errsz,"implausible sector size %u",ss); return -1; }
    (void)sector_guess;
    if (gfc_fseek(f,0,SEEK_SET)||fread(sb,1,ss,f)!=ss){ snprintf(err,errsz,"short read of superblock"); return -1; }
    if (get_u32(sb+SB_GFC_MAGIC)!=GFC_SB_MAGIC){ snprintf(err,errsz,"bad GFC magic (not a G-format image)"); return -1; }

    uint64_t total_bytes = get_u64(sb+SB_TotalSectors) * ss;
    uint32_t cluster = 1u << (head[SB_Log2SectorSize]+sb[SB_Log2SecsPerClu]);
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

#define FAIL(...) do{ fprintf(stderr,"FAIL: "); fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); nerr++; }while(0)

static int cmd_check(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool check <image>\n"); return 2; }
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    int nerr=0;

    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"FAIL: %s\n",err); fclose(f); return 1; }

    /* 1. superblock checksum + secondary copy */
    if (get_u32(sb+g.sector_size-4)!=gfc_struct_check(sb,g.sector_size)) FAIL("superblock checksum");
    { uint8_t *sb2=malloc(g.sector_size);
      if (read_sector(f,&g,1,sb2)) FAIL("cannot read secondary superblock");
      else if (memcmp(sb,sb2,g.sector_size)) FAIL("secondary superblock differs from primary");
      free(sb2); }

    /* 2. geometry vs image length */
    if (gfc_fseek(f,0,SEEK_END)==0){
        uint64_t len=(uint64_t)gfc_ftell(f);
        if (len != g.total_sectors*g.sector_size) FAIL("image length %llu != TotalSectors*sector %llu",
            (unsigned long long)len,(unsigned long long)(g.total_sectors*g.sector_size));
    }
    if (get_u64(sb+SB_AGCount)!=g.agcount) FAIL("AGCount field %llu != computed %llu",
        (unsigned long long)get_u64(sb+SB_AGCount),(unsigned long long)g.agcount);

    /* 3. walk root: validate object records, collect AG0 object runs (cluster units) */
    uint32_t clu_bytes = 1u<<g.log2_bpmb;
    uint64_t real0 = ag_real_clusters(&g,0);
    uint8_t *root = calloc(1,clu_bytes);
    if (read_run(f,&g,ag_data_start(&g,0),1,root)) FAIL("cannot read root directory");
    if (get_u32(root+OBJ_Magic)!=GFC_OBJ_MAGIC) FAIL("root object bad magic");
    if (root[OBJ_Type]!=OBJ_TYPE_DIR) FAIL("root object is not a directory");
    if (get_u32(root+OBJ_HdrCheck)!=sum_words(root,OBJ_HdrCheck)) FAIL("root object header checksum");
    uint32_t nent = get_u32(root+OBJ_HDR_BYTES);

    uint64_t *run_start=malloc((nent+1)*sizeof(uint64_t)); /* AG0 cluster index */
    uint64_t *run_count=malloc((nent+1)*sizeof(uint64_t));
    int nruns=0;
    for (uint32_t e=0;e<nent;e++){
        const uint8_t *de=root+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        uint64_t st=get_u64(de+DE_StartSector);
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        uint8_t *oh=calloc(1,clu_bytes);
        if (read_run(f,&g,st,1,oh)){ FAIL("entry '%s': cannot read object",nm); free(oh); continue; }
        if (get_u32(oh+OBJ_Magic)!=GFC_OBJ_MAGIC) FAIL("entry '%s': object bad magic",nm);
        if (get_u32(oh+OBJ_HdrCheck)!=sum_words(oh,OBJ_HdrCheck)) FAIL("entry '%s': object header checksum",nm);
        if (get_u64(oh+OBJ_StartSector)!=st) FAIL("entry '%s': object StartSector mismatch",nm);
        uint64_t cc=get_u64(oh+OBJ_ClusterCount), cidx=st>>g.log2_secs_per_clu;
        if (cidx+cc>real0) FAIL("entry '%s': object run out of AG0 range",nm);
        else { run_start[nruns]=cidx; run_count[nruns]=cc; nruns++; }
        free(oh);
    }

    /* 4. each AG header and map (object runs applied to AG0) */
    uint8_t *h=malloc(g.sector_size), *map=malloc((size_t)g.map_zones*g.sector_size);
    uint8_t *exp=malloc((size_t)g.map_zones*g.sector_size);
    for (uint64_t i=0;i<g.agcount && nerr<50;i++){
        if (read_sector(f,&g,ag_header_sector(&g,i),h)){ FAIL("read AG %llu header",(unsigned long long)i); continue; }
        if (get_u32(h+AGH_Magic)!=GFC_AGH_MAGIC) FAIL("AG %llu bad magic",(unsigned long long)i);
        if (get_u64(h+AGH_AGNumber)!=i) FAIL("AG %llu number field wrong",(unsigned long long)i);
        if (get_u64(h+AGH_BaseSector)!=ag_base(&g,i)) FAIL("AG %llu base sector",(unsigned long long)i);
        if (get_u32(h+g.sector_size-4)!=gfc_struct_check(h,g.sector_size)) FAIL("AG %llu header checksum",(unsigned long long)i);

        /* read map, verify zone & cross checks independently */
        uint8_t cross=0;
        for (uint16_t z=0; z<g.map_zones; z++){
            if (read_sector(f,&g,ag_map_start(&g,i)+z, map+(uint32_t)z*g.sector_size)){ FAIL("AG %llu read map zone %u",(unsigned long long)i,z); continue; }
            uint8_t *zs=map+(uint32_t)z*g.sector_size;
            uint8_t want=gfc_zone_check(zs,g.sector_size);
            if (zs[0]!=want) FAIL("AG %llu zone %u ZoneCheck (got %02x want %02x)",(unsigned long long)i,z,zs[0],want);
            cross ^= zs[1];
        }
        if (cross!=0xFF) FAIL("AG %llu CrossCheck EOR=%02x (want ff)",(unsigned long long)i,cross);

        /* expected = structural reserved (+root) [+ object runs in AG0] */
        uint64_t cfree,ctot,lg;
        memset(exp,0,(size_t)g.map_zones*g.sector_size);
        ag_reserved_bits(exp,&g,i,1,&cfree,&ctot);
        if (i==0){
            for (int r=0;r<nruns;r++)
                for (uint64_t k=0;k<run_count[r];k++) map_set(exp,&g,(uint32_t)(run_start[r]+k));
            map_scan_free(exp,&g,real0,&cfree,&lg);   /* recount after objects */
        }
        for (uint32_t c=0;c<g.clusters_per_ag;c++)
            if (map_get(map,&g,c)!=map_get(exp,&g,c)){ FAIL("AG %llu cluster %u allocation bit mismatch (map vs objects)",(unsigned long long)i,c); break; }
        if (get_u64(h+AGH_ClustersFree)!=cfree) FAIL("AG %llu ClustersFree %llu != expected %llu",
            (unsigned long long)i,(unsigned long long)get_u64(h+AGH_ClustersFree),(unsigned long long)cfree);
        if (get_u64(h+AGH_ClustersTotal)!=ctot) FAIL("AG %llu ClustersTotal mismatch",(unsigned long long)i);
    }
    free(h); free(map); free(exp); free(root); free(run_start); free(run_count);

    fclose(f);
    if (nerr){ printf("CHECK FAILED: %d error(s)\n",nerr); return 1; }
    printf("CHECK OK: %llu AGs, %llu sectors, all structures consistent\n",
           (unsigned long long)g.agcount,(unsigned long long)g.total_sectors);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    if (argc<1){ fprintf(stderr,"usage: gfctool ls <image>\n"); return 2; }
    FILE *f=fopen(argv[0],"rb"); if(!f){ perror("fopen"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint8_t *root=calloc(1,1u<<g.log2_bpmb);
    if (read_run(f,&g,ag_data_start(&g,0),1,root)){ fprintf(stderr,"cannot read root\n"); free(root); fclose(f); return 1; }
    uint32_t nent=get_u32(root+OBJ_HDR_BYTES);
    printf("$ (%u object%s)\n", nent, nent==1?"":"s");
    for (uint32_t e=0;e<nent;e++){
        const uint8_t *de=root+OBJ_HDR_BYTES+4+(size_t)e*DIRENT_BYTES;
        char nm[13]; memcpy(nm,de+DE_Name,12); nm[12]=0;
        for(int k=11;k>=0&&(nm[k]==' '||!nm[k]);k--) nm[k]=0;
        printf("  %-12s  %-4s  %10llu bytes  load=%08x exec=%08x\n",
               nm, de[DE_Type]==OBJ_TYPE_DIR?"dir":"file",
               (unsigned long long)get_u64(de+DE_Length), get_u32(de+DE_Load), get_u32(de+DE_Exec));
    }
    free(root); fclose(f); return 0;
}

static int cmd_mkfile(int argc, char **argv)
{
    if (argc<3){ fprintf(stderr,"usage: gfctool mkfile <image> <name> <srcfile>\n"); return 2; }
    const char *imgp=argv[0], *name=argv[1], *srcp=argv[2];
    FILE *f=fopen(imgp,"rb+"); if(!f){ perror("fopen image"); return 1; }
    gfc_geom g; char err[128]; uint8_t sb[4096];
    if (load_geom(f,&g,sb,4096,err,sizeof err)){ fprintf(stderr,"%s\n",err); fclose(f); return 1; }
    uint32_t clu_bytes=1u<<g.log2_bpmb;

    /* read source file */
    FILE *s=fopen(srcp,"rb"); if(!s){ perror("fopen src"); fclose(f); return 1; }
    gfc_fseek(s,0,SEEK_END); uint64_t fsize=(uint64_t)gfc_ftell(s); gfc_fseek(s,0,SEEK_SET);
    uint64_t need=ceil_div(fsize+OBJ_HDR_BYTES, clu_bytes);

    /* find a contiguous run in AG0 */
    uint64_t real0=ag_real_clusters(&g,0);
    uint32_t first=ag_first_data_cluster(&g,0);
    uint8_t *map=malloc((size_t)g.map_zones*g.sector_size);
    for (uint16_t z=0;z<g.map_zones;z++) read_sector(f,&g,ag_map_start(&g,0)+z,map+(uint32_t)z*g.sector_size);
    int64_t run=map_find_run(map,&g,first,real0,need);
    if (run<0){ fprintf(stderr,"no contiguous space for %llu cluster(s) in AG0\n",(unsigned long long)need); free(map); fclose(s); fclose(f); return 1; }

    /* mark allocated, refresh checks + AG0 header */
    for (uint64_t k=0;k<need;k++) map_set(map,&g,(uint32_t)(run+k));
    map_finalise_checks(map,&g);
    uint64_t cfree,lg; map_scan_free(map,&g,real0,&cfree,&lg);
    for (uint16_t z=0;z<g.map_zones;z++) write_sector(f,&g,ag_map_start(&g,0)+z,map+(uint32_t)z*g.sector_size);
    { uint8_t *h=malloc(g.sector_size); read_sector(f,&g,ag_header_sector(&g,0),h);
      put_u64(h+AGH_ClustersFree,cfree); put_u64(h+AGH_LargestFreeRun,lg);
      put_u32(h+g.sector_size-4,gfc_struct_check(h,g.sector_size));
      write_sector(f,&g,ag_header_sector(&g,0),h); free(h); }

    /* write the object (header + data) */
    uint64_t start_sec=(uint64_t)run<<g.log2_secs_per_clu;
    uint8_t *obj=calloc(need,clu_bytes);
    uint64_t objid=((uint64_t)0<<GFC_OBJID_LOCALBITS)|(uint64_t)run;   /* AG 0 */
    build_obj_header(obj,objid,OBJ_TYPE_FILE,0x03,0xFFFFFD00u,0,fsize,start_sec,need,name);
    if (fsize) { if (fread(obj+OBJ_HDR_BYTES,1,fsize,s)!=fsize){ fprintf(stderr,"short read of src\n"); } }
    write_run(f,&g,start_sec,need,obj);
    free(obj); fclose(s);

    /* append directory entry to root */
    uint8_t *root=calloc(1,clu_bytes);
    read_run(f,&g,ag_data_start(&g,0),1,root);
    uint32_t nent=get_u32(root+OBJ_HDR_BYTES);
    uint64_t root_cap=get_u64(root+OBJ_ClusterCount)*clu_bytes;
    if (OBJ_HDR_BYTES+4+(uint64_t)(nent+1)*DIRENT_BYTES > root_cap){
        fprintf(stderr,"root directory full (%u entries)\n",nent); free(root); free(map); fclose(f); return 1; }
    uint8_t *de=root+OBJ_HDR_BYTES+4+(size_t)nent*DIRENT_BYTES;
    memset(de,0,DIRENT_BYTES);
    memset(de+DE_Name,' ',DE_NameLen);
    { size_t n=strlen(name); if(n>DE_NameLen)n=DE_NameLen; memcpy(de+DE_Name,name,n); }
    de[DE_Type]=OBJ_TYPE_FILE; de[DE_Attrs]=0x03;
    put_u32(de+DE_Load,0xFFFFFD00u); put_u32(de+DE_Exec,0);
    put_u64(de+DE_Length,fsize); put_u64(de+DE_StartSector,start_sec);
    put_u32(root+OBJ_HDR_BYTES,nent+1);
    put_u64(root+OBJ_Length,4+(uint64_t)(nent+1)*DIRENT_BYTES);
    put_u32(root+OBJ_HdrCheck,sum_words(root,OBJ_HdrCheck));
    write_run(f,&g,ag_data_start(&g,0),1,root);
    free(root); free(map); fclose(f);

    printf("added '%s' (%llu bytes, %llu cluster%s) at sector %llu\n",
           name,(unsigned long long)fsize,(unsigned long long)need,need==1?"":"s",(unsigned long long)start_sec);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc<2){
        fprintf(stderr,
          "gfctool - FileCore G-format reference (bounty #40)\n"
          "usage:\n"
          "  gfctool format <image> [--size N] [--sector N] [--ag-size N] [--bpmb N] [--name STR]\n"
          "  gfctool mkfile <image> <name> <srcfile>\n"
          "  gfctool ls     <image>\n"
          "  gfctool check  <image>\n"
          "  gfctool info   <image>\n"
          "sizes accept K/M/G/T/E suffixes.\n");
        return 2;
    }
    if (!strcmp(argv[1],"format")) return cmd_format(argc-2,argv+2);
    if (!strcmp(argv[1],"mkfile")) return cmd_mkfile(argc-2,argv+2);
    if (!strcmp(argv[1],"ls"))     return cmd_ls    (argc-2,argv+2);
    if (!strcmp(argv[1],"check"))  return cmd_check (argc-2,argv+2);
    if (!strcmp(argv[1],"info"))   return cmd_info  (argc-2,argv+2);
    fprintf(stderr,"unknown command: %s\n",argv[1]);
    return 2;
}
