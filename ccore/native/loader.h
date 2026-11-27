/* The native app's loader (ticket NUI1): a link's geometry made into renderer parts on threads of its
 * own, so the UI never waits for a decode or an upload, and one link at a time per thread holds its
 * transient arrays (the memory peak of a 17-link project is one or two links' worth, not all).
 *
 * The session is not thread-safe: every access to it goes through one lock. The UI thread takes it
 * as the writer around each call (nv_ses_wr / nv_ses_unwr; it waits at most one batch of a loader:
 * BATCH elements, a few ms); a loader takes it as a reader per batch, finds its link again by uid and
 * checks it is the same data (its view file, IFC scene or model), else the job is cancelled.
 *
 * A link read in place (SIZE3: a cache mapped, `vf->lazy`) is decoded here from its cache's mesh
 * blocks into the loader's own scratch, never into the session's (cc_link_resident is not called):
 * the session's decode budget stays for the calls that need geometry (measure, a box, a volume). */
#ifndef BIMBLE_NATIVE_LOADER_H
#define BIMBLE_NATIVE_LOADER_H
#include <stdint.h>
#include <SDL3/SDL.h>
#include "native.h"
#include "render.h"

struct cc_session;
typedef struct nv_loader nv_loader;

enum { NV_JOB_PROXIES = 1, NV_JOB_DETAIL = 2 };

typedef struct nv_job {
    /* in */
    int kind;
    struct cc_session *ses;
    uint32_t uid;        /* the link's (links.h cc_link.uid) */
    const void *ident;   /* its data when queued (vf, ifc or model pointer): another one cancels */
    uint32_t elem0;      /* its elements' first global number (DETAIL) */
    double origin0[3];   /* the scene origin */
    nv_load_opts lo;     /* its placement and colour */
    int with_proxies;    /* DETAIL: its cache's proxies too (far cells drawn as proxies) */
    int tag;             /* the caller's */
    /* out */
    nv_part *part;       /* NULL: none (err, or nothing to draw: err empty) */
    char err[256];
    int cancelled;
    double ms;           /* the job's wall time */
    uint64_t peak_bytes; /* its CPU arrays before the upload */
    struct nv_job *next;
} nv_job;

nv_loader *nv_loader_new(SDL_GPUDevice *dev, int threads);
void nv_loader_free(nv_loader *L);
/* The session's lock, as the writer (the UI thread, around every call into the session). */
void nv_ses_wr(nv_loader *L);
void nv_ses_unwr(nv_loader *L);
/* Queued jobs run in order; nv_loader_idle: how many threads wait for one. */
void nv_loader_submit(nv_loader *L, nv_job *j);
int nv_loader_idle(nv_loader *L);
int nv_loader_busy(nv_loader *L); /* jobs queued or running */
/* A finished job (the caller frees it with nv_job_free after taking its part), NULL none. */
nv_job *nv_loader_take(nv_loader *L);
void nv_job_free(SDL_GPUDevice *dev, nv_job *j);
/* Cancels every job of link `uid` (0: every job). */
void nv_loader_cancel(nv_loader *L, uint32_t uid);
/* The identity of a session link's data (NULL: not ready). Under the lock. */
const void *nv_link_ident(const struct cc_session *s, int i);
#endif
