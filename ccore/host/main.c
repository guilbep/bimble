/* bimble-ccore: the C core's native host. Serves the twin protocol of
 * crates/bimble-viewer/src/serve.rs on 127.0.0.1 for the Vue lab (`?native=1&port=&token=`).
 *   bimble-ccore [port] [model]
 *                             port: argv[1], else BIMBLE_SERVE (as twin.rs:106-114)
 *                             model: argv[2], else CCORE_MODEL; a .bimble/.json text form, or an
 *                             .ifc (imported); none: the host answers as a viewer with no model
 *   BIMBLE_TOKEN              the token; random when unset or empty (twin.rs:115-118)
 *   BIMBLE_ORIGINS            extra allowed Origins, comma separated, beside the twin's two
 *                             (twin.rs:17), e.g. a Vite dev server on another port */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "session.h"

/* 32 hex digits, as twin.rs `random_token` (twin.rs:88-99): /dev/urandom, else the clock. */
static void random_hex(char out[33]) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t k = fd >= 0 ? read(fd, b, sizeof b) : -1;
    if (fd >= 0) close(fd);
    if (k != (ssize_t)sizeof b) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long x = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
        x ^= (unsigned long long)getpid() << 32;
        for (int i = 0; i < 16; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17; /* xorshift64 */
            b[i] = (unsigned char)x;
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[2 * i] = hex[b[i] >> 4], out[2 * i + 1] = hex[b[i] & 15];
    out[32] = 0;
}

static int parse_port(const char *s, unsigned *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++; /* `trim()` before parse (twin.rs:108) */
    char *end;
    if (!*s || *s == '-') return 0;
    unsigned long v = strtoul(s, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (*end || v > 65535) return 0;
    *out = (unsigned)v;
    return 1;
}

int main(int argc, char **argv) {
    const char *ps = argc > 1 ? argv[1] : getenv("BIMBLE_SERVE");
    unsigned port;
    if (!ps) {
        fprintf(stderr, "usage: bimble-ccore <port> [model] (or BIMBLE_SERVE=<port>, CCORE_MODEL=<model>)\n");
        return 2;
    }
    if (!parse_port(ps, &port)) {
        fprintf(stderr, "bimble-ccore: \"%s\" is not a port\n", ps);
        return 2;
    }
    const char *model = argc > 2 ? argv[2] : getenv("CCORE_MODEL");
    if (model && *model) {
        cc_error err;
        if (cc_session_open(model, &err)) {
            fprintf(stderr, "bimble-ccore: %s: %s\n", model, err.msg);
            return 1;
        }
    }
    static char token[33], epoch[33];
    const char *t = getenv("BIMBLE_TOKEN");
    if (!t || !*t) {
        random_hex(token);
        t = token;
    }
    random_hex(epoch);

    cc_host_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = (uint16_t)port;
    cfg.token = t;
    cfg.epoch = epoch;
    cfg.origins[cfg.n_origins++] = "http://localhost:5173";
    cfg.origins[cfg.n_origins++] = "http://127.0.0.1:5173";
    static char extra[1024];
    const char *e = getenv("BIMBLE_ORIGINS");
    if (e && strlen(e) < sizeof extra) {
        strcpy(extra, e);
        for (char *tok = strtok(extra, ","); tok && cfg.n_origins < CC_HOST_MAX_ORIGINS; tok = strtok(NULL, ","))
            if (*tok && strlen(tok) < 256) cfg.origins[cfg.n_origins++] = tok;
    }
    return cc_host_run(&cfg);
}
