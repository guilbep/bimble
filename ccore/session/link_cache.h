/* LINKF (ADR 0079, DEVIATIONS 37): a linked IFC read once, Revit's way ("Link IFC" writes
 * <file>.ifc.RVT beside the IFC): the light read-only form beside it, and a plain-text report.
 *   <file>.ifc.bimbleview  the reader's scene as a view file (viewfile.h, copies shared), raw (not
 *                          gzipped: read without inflating), plus a section SRCE: the source
 *                          {"base_point_mm", "bytes", "ends", "format", "mtime_s"}; "ends" is the BLAKE3 of the
 *                          IFC's first MiB, its last MiB and its size (8 bytes LE), in hex
 *   <file>.ifc.log.txt     the conversion report: schema, counts per IFC class, products with and
 *                          without a body, what was skipped and why (the reader's warnings),
 *                          times, sizes, where the cache went
 * When the IFC's folder cannot be written, both go to the platform's cache folder
 * (pf->cache_dir: links/<16 hex of BLAKE3(path)>-<file name>…), and the report says so.
 * The next link of the same IFC loads the cache instead of the IFC when its SRCE matches the IFC
 * now: size and time by stat, the ends hash by two reads of 1 MiB (pf->read_range; when the IFC
 * cannot be read, size and time alone, said in the job's note). A cache that does not match, is
 * damaged or is not a view file is not trusted: read as any untrusted file, then replaced.
 * Files under CC_LINK_CACHE_MIN bytes get no cache (read as before): reading them is as fast.
 *
 * The job (cc_link_job_run) touches only the job and calls only the platform: a carrier may run it
 * on its own thread (pf->spawn) while the session goes on; the session reads `pg` and `finished`
 * with relaxed atomics and takes the results once `finished` is 1. */
#ifndef CCORE_SESSION_LINK_CACHE_H
#define CCORE_SESSION_LINK_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include "ifc_read.h"
#include "platform.h"
#include "viewfile.h"
#include "hlod.h"

#define CC_LINK_CACHE_MIN ((uint64_t)1 << 20) /* bytes; below, no cache (ADR 0079) */
#define CC_LINK_CACHE_FORMAT "bimble-link-cache 5" /* 2: SRCE names the base point; 3 (M1LINKS): a section PROP; 4 (LNK3D): a section HLOD; 5 (SIZE2): HLOD gzipped within a budget, PRPZ */
/* SIZE2: the proxies' byte budget per link (cc_link_hlod_cap): 15 % of its geometry, within these. */
#define CC_LINK_HLOD_MIN ((uint64_t)256 << 10)
#define CC_LINK_HLOD_MAX ((uint64_t)32 << 20)
/* SIZE2: the most triangles of one link cell's proxy (3D2's 65,536 for the open model): the root of a large
 * link stays within the budget's floor, so the coarsest level is always kept. */
#define CC_LINK_HLOD_PROXY_TRIS 4096u

enum { CC_JOB_OK = 0, CC_JOB_FAILED = -1, CC_JOB_REFUSED = -2, CC_JOB_CANCELLED = -3 };

typedef struct cc_link_job {
    const cc_platform *pf;
    char *path;          /* malloc'd: the file linked (an .ifc, a .bimbleview) */
    uint64_t cache_min;  /* CC_LINK_CACHE_MIN, or less (a test) */
    int no_beside;       /* 1: never beside the IFC, only the cache folder (a folder not to write in) */
    cc_ifc_progress pg;  /* 0..1000; cancel */
    int finished;        /* 0 running, 1 the results below are written, 2 orphaned (atomic) */
    /* results */
    int rc;              /* CC_JOB_* */
    cc_error err;
    cc_viewfile *vf;     /* the light scene (malloc'd), or */
    cc_ifc_scene *ifc;   /* a small IFC read as it is (no cache) */
    char *psets;         /* M1LINKS: its property sets table (ifc_psets.h, malloc'd), or NULL */
    size_t npsets;
    int from_cache;      /* 1: the cache was loaded, the IFC not read */
    char cache[4096];    /* the cache's path ("" none) */
    char log[4096];      /* the report's path ("" none) */
    char note[256];      /* one line for the panel: "cache in the user's cache folder", … */
    uint64_t bytes, cache_bytes;
    int64_t mtime_s, cache_mtime_s;
    double ms;           /* the whole job */
    /* LNK3D (ADR 0078 amendment 3): the proxies baked into the cache (section HLOD) */
    uint64_t hlod_cells, hlod_tris, hlod_bytes;
    double hlod_ms;
    char hlod_why[160];  /* "" or why none were baked */
    /* SIZE2: the geometry section's bytes (the proxies' budget), the cells left without a proxy, the HLOD's
     * stored (gzipped) bytes, the property sets' bytes as read, each section's bytes (HEAD, META, GEOR, ELEM,
     * SRCE, HLOD, PROP, PRPZ) */
    uint64_t geom_bytes, hlod_stored, psets_raw, sections[8];
    uint32_t hlod_dropped;
    /* SIZE3: the cache mapped (pf->map_file) and `vf` read in place over it (viewfile.h
     * cc_viewfile_read_mapped): the link takes the mapping with the view file and unmaps it after
     * freeing it. NULL: `vf` owns its bytes (no mapping: the wasm module, a gzipped cache, a failure). */
    void *map;
} cc_link_job;

/* A new job for `path` (copied). NULL out of memory. */
cc_link_job *cc_link_job_new(const cc_platform *pf, const char *path);
/* The work: on any thread. */
void cc_link_job_run(void *job);
/* Frees the job and what it still holds (results not taken). Not while it runs. */
void cc_link_job_free(cc_link_job *j);
/* 1 once the job's results can be taken (acquire). */
int cc_link_job_done(cc_link_job *j);
/* The session goes while the job may run: cancelled, and freed by whichever side ends last. */
void cc_link_job_orphan(cc_link_job *j);

/* SIZE2: the proxies' budget of a link whose geometry section is `geom_bytes` long, and the budget applied:
 * the finest cells' proxies dropped (the coarsest kept) until the rest fit `cap` bytes; the cells dropped. */
uint64_t cc_link_hlod_cap(uint64_t geom_bytes);
uint32_t cc_link_hlod_budget(cc_hlod *h, uint64_t cap);

/* The SRCE text of an IFC now (`head`, `tail`: its first and last MiB, NULL when unread), into
 * out[0..n). 0 ok. */
int cc_link_cache_source(uint64_t bytes, int64_t mtime_s, const char *head, size_t nh, const char *tail, size_t nt,
                         const double *base_mm, char *out, size_t n);
/* LINKF: the IFC's base point a cache's SRCE names ("base_point_mm"): 0, or -1 when none. */
int cc_link_cache_base_point(const char *src, size_t n, double out[3]);
/* Whether `src` (a cache's SRCE) matches the IFC: 1 yes; with ends_known 0, size and time only. */
int cc_link_cache_matches(const char *src, size_t n, uint64_t bytes, int64_t mtime_s, const char *ends_hex, int ends_known);
#endif
