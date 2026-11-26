/* Parallel jobs (L0): run N independent tasks on a pool of threads, results deterministic.
 * core-architecture.md §5: parallel pure functions on a frozen model, one scratch arena per
 * worker. Natively pthreads (tcc and clang); in the freestanding wasm build every job runs on the
 * calling thread, in task order.
 *
 * Determinism is the caller's: each task writes only its own result slot (task index), and the
 * caller merges the slots in index order after cc_jobs_run returns. Per-worker state (a reader,
 * a scratch arena) is indexed by `worker`, in [0, cc_jobs_workers(ntasks)). Worker 0 is the
 * calling thread. Tasks are taken in index order from a shared counter (dynamic scheduling).
 *
 * The one global of L0: the thread count (cc_jobs_set_threads; default: the CC_THREADS
 * environment variable, else the online processors, at most CC_JOBS_MAX). */
#ifndef CCORE_JOBS_H
#define CCORE_JOBS_H
#include <stddef.h>

#define CC_JOBS_MAX 64

#if !defined(__wasm__)
#define CC_JOBS_THREADS 1
#include <stdatomic.h> /* tcc: __ATOMIC_* (the builtins below work in tcc and clang) */
/* Relaxed atomics on size_t / int for shared counters (read.rs Budget). */
#define cc_atomic_load(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define cc_atomic_store(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define cc_atomic_cas(p, expected, desired) \
    __atomic_compare_exchange_n((p), (expected), (desired), 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)
#else
#define CC_JOBS_THREADS 0
#define cc_atomic_load(p) (*(p))
#define cc_atomic_store(p, v) (*(p) = (v))
#define cc_atomic_cas(p, expected, desired) (*(p) == *(expected) ? (*(p) = (desired), 1) : (*(expected) = *(p), 0))
#endif

typedef void (*cc_job_fn)(void *ctx, size_t task, size_t worker);

/* Threads cc_jobs_run uses (>= 1). */
size_t cc_jobs_threads(void);
/* 0: the default again. Not while jobs run. */
void cc_jobs_set_threads(size_t n);
/* Workers a run of `ntasks` tasks uses: min(threads, ntasks), at least 1. Size per-worker state
 * with it. */
size_t cc_jobs_workers(size_t ntasks);
/* Runs fn(ctx, task, worker) for every task in [0, ntasks), returns when all are done. A thread
 * that cannot be started leaves its share to the others (never fails). */
void cc_jobs_run(size_t ntasks, cc_job_fn fn, void *ctx);
#endif
