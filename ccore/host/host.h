/* The native host's HTTP server: the transport of crates/bimble-viewer/src/serve.rs, single
 * threaded over poll(). */
#ifndef CCORE_HOST_HOST_H
#define CCORE_HOST_HOST_H
#include <stdint.h>

#define CC_HOST_MAX_ORIGINS 8

typedef struct {
    uint16_t port;      /* 0: any free port */
    const char *token;  /* the `t=` query parameter every request but OPTIONS must carry */
    const char *epoch;  /* per process, sent with each change (twin.rs:121, `random_token`) */
    const char *origins[CC_HOST_MAX_ORIGINS]; /* allowed `Origin` values */
    int n_origins;
} cc_host_config;

/* Binds 127.0.0.1:port, prints "bimble-ccore: listening <port> token <token>" on stderr,
 * publishes the first change and serves until SIGTERM or SIGINT. Returns 0 then, 1 when it
 * cannot listen. */
int cc_host_run(const cc_host_config *cfg);
#endif
