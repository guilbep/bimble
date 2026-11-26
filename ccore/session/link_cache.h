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

#define CC_LINK_CACHE_MIN ((uint64_t)1 << 20) /* bytes; below, no cache (ADR 0079) */
#define CC_LINK_CACHE_FORMAT "bimble-link-cache 3" /* 2: SRCE names the base point; 3 (M1LINKS): a section PSET */

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

/* The SRCE text of an IFC now (`head`, `tail`: its first and last MiB, NULL when unread), into
 * out[0..n). 0 ok. */
int cc_link_cache_source(uint64_t bytes, int64_t mtime_s, const char *head, size_t nh, const char *tail, size_t nt,
                         const double *base_mm, char *out, size_t n);
/* LINKF: the IFC's base point a cache's SRCE names ("base_point_mm"): 0, or -1 when none. */
int cc_link_cache_base_point(const char *src, size_t n, double out[3]);
/* Whether `src` (a cache's SRCE) matches the IFC: 1 yes; with ends_known 0, size and time only. */
int cc_link_cache_matches(const char *src, size_t n, uint64_t bytes, int64_t mtime_s, const char *ends_hex, int ends_known);
#endif
