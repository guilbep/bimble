/* Parallel jobs: see jobs.h. */
#include "jobs.h"

static size_t g_threads; /* 0: default */

#if CC_JOBS_THREADS
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static size_t default_threads(void) {
    const char *e = getenv("CC_THREADS");
#ifdef _WIN32
    long n = e && *e ? strtol(e, NULL, 10) : pthread_num_processors_np(); /* WIN1: no sysconf (winpthreads) */
#else
    long n = e && *e ? strtol(e, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    if (n > CC_JOBS_MAX) n = CC_JOBS_MAX;
    return (size_t)n;
}

typedef struct {
    cc_job_fn fn;
    void *ctx;
    size_t ntasks;
    size_t next; /* next task to take (atomic) */
} Run;

typedef struct {
    Run *run;
    size_t worker;
} Worker;

static void work(Run *r, size_t worker) {
    for (;;) {
        size_t t = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
        if (t >= r->ntasks) return;
        r->fn(r->ctx, t, worker);
    }
}

static void *thread_main(void *p) {
    Worker *w = p;
    work(w->run, w->worker);
    return NULL;
}

void cc_jobs_run(size_t ntasks, cc_job_fn fn, void *ctx) {
    size_t nw = cc_jobs_workers(ntasks);
    Run r = {fn, ctx, ntasks, 0};
    if (nw <= 1) {
        work(&r, 0);
        return;
    }
    pthread_t th[CC_JOBS_MAX];
    Worker ws[CC_JOBS_MAX];
    int started[CC_JOBS_MAX] = {0};
    for (size_t k = 1; k < nw; k++) {
        ws[k].run = &r, ws[k].worker = k;
        started[k] = pthread_create(&th[k], NULL, thread_main, &ws[k]) == 0;
    }
    work(&r, 0);
    for (size_t k = 1; k < nw; k++)
        if (started[k]) pthread_join(th[k], NULL);
}
#else
static size_t default_threads(void) { return 1; }

void cc_jobs_run(size_t ntasks, cc_job_fn fn, void *ctx) {
    for (size_t t = 0; t < ntasks; t++) fn(ctx, t, 0);
}
#endif

size_t cc_jobs_threads(void) { return CC_JOBS_THREADS ? (g_threads ? g_threads : default_threads()) : 1; }

void cc_jobs_set_threads(size_t n) { g_threads = n > CC_JOBS_MAX ? CC_JOBS_MAX : n; }

size_t cc_jobs_workers(size_t ntasks) {
    size_t t = cc_jobs_threads();
    if (t > ntasks) t = ntasks;
    return t ? t : 1;
}
