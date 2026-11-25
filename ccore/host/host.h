/* The native host (L6 carrier): the HTTP server, the transport of
 * crates/bimble-viewer/src/serve.rs, single threaded over poll(), and the native platform. */
#ifndef CCORE_HOST_HOST_H
#define CCORE_HOST_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "session.h"

#define CC_HOST_MAX_ORIGINS 8

typedef struct {
    uint16_t port;      /* 0: any free port */
    const char *token;  /* the `t=` query parameter every request but OPTIONS must carry */
    const char *epoch;  /* per process, sent with each change (twin.rs:121, `random_token`) */
    const char *origins[CC_HOST_MAX_ORIGINS]; /* allowed `Origin` values */
    int n_origins;
    int warm;           /* when idle, build the scene and /mesh of a new rev before they are asked for */
    cc_session *session; /* the session the calls and /mesh serve (main.c's default session) */
} cc_host_config;

/* Binds 127.0.0.1:port, prints "bimble-ccore: listening <port> token <token>" on stderr,
 * publishes the first change and serves until SIGTERM or SIGINT. Returns 0 then, 1 when it
 * cannot listen. */
int cc_host_run(const cc_host_config *cfg);

/* The change feed (server.c): publishes `json` if it differs from the last one published,
 * bumps seq and answers every waiting GET /change (twin.rs `Twin::publish`). */
void cc_host_publish_change(const char *json, size_t n);

/* The native cc_platform (platform_native.c): POSIX files, CLOCK_MONOTONIC, time(), /dev/urandom,
 * stderr. Static, never freed. */
const cc_platform *cc_platform_native(void);
#endif
