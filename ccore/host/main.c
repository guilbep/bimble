/* bimble-ccore: the C core's native host. Serves the twin protocol of
 * crates/bimble-viewer/src/serve.rs on 127.0.0.1 for the Vue lab (`?native=1&port=&token=`).
 *   bimble-ccore [--session[=file]] [--warm] [port] [model]
 *   bimble-ccore mcp [--read-only] <model>   an MCP server over stdio instead (mcp.h)
 *                             --warm: when idle after a load or a change, build the scene and the
 *                             /mesh bytes of the new rev before the page asks (dev.sh uses it)
 *                             --session: keep the session (model edits, undo steps, selection,
 *                             rev) in a file across restarts (session.h): <model>.ccore-session,
 *                             or `file`; also on when CCORE_SESSION names a file
 *                             port: argv[1], else BIMBLE_SERVE (as twin.rs:106-114)
 *                             model: argv[2], else CCORE_MODEL; a .bimble (v1 text or v2 binary),
 *                             a .json, or an .ifc (imported); none: the host answers as a viewer
 *                             with no model. The calls open, create, save and close models later
 *                             (`action new|open|save|save_as|export_ifc4|export_ifc2x3|close`,
 *                             session/calls.c file_action); the session file follows them
 *   bimble-ccore open [--port N] [--no-browser] [--web dir] [--session[=file]] [model]
 *                             the desktop (ADR 0069 rule 2, ticket R1): also serves the built lab
 *                             from <exe dir>/web/ (or --web; static.h), on a free port of
 *                             127.0.0.1 (or N), prints the lab's URL on stdout and starts xdg-open
 *                             (`open` on macOS)
 *                             on it (not with --no-browser). Always --warm.
 *   bimble-ccore --version    prints ccore/VERSION (build.sh passes it as CC_RELEASE_VERSION)
 *   BIMBLE_TOKEN              the token; random when unset or empty (twin.rs:115-118)
 *   BIMBLE_ORIGINS            extra allowed Origins, comma separated, beside the twin's two
 *                             (twin.rs:17), e.g. a Vite dev server on another port */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* realpath and the dyld header beside _POSIX_C_SOURCE */
#endif
#include <fcntl.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "mcp.h"
#include "session.h"
#include "static.h"

#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath: macOS has no /proc/self/exe */
#define BROWSER_OPENER "open"
#else
#define BROWSER_OPENER "xdg-open"
#endif

#ifndef CC_RELEASE_VERSION
#define CC_RELEASE_VERSION "dev" /* build.sh passes ccore/VERSION */
#endif

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

/* `open`: the lab's URL once the port is known (cc_host_config.on_listen). */
typedef struct {
    const char *token;
    int browser;
} OpenCtx;

static void url_put_token(Buf *b, const char *t) { /* encodeURIComponent's unreserved set */
    static const char hex[] = "0123456789ABCDEF";
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c))
            buf_putc(b, (char)c);
        else
            buf_putc(b, '%'), buf_putc(b, hex[c >> 4]), buf_putc(b, hex[c & 15]);
    }
}

static void on_listen(uint16_t port, void *ctx) {
    const OpenCtx *o = ctx;
    Buf url;
    buf_init(&url);
    buf_puts(&url, "http://127.0.0.1:");
    buf_u64(&url, port);
    buf_puts(&url, "/?native=1&gl=1&port=");
    buf_u64(&url, port);
    buf_puts(&url, "&token=");
    url_put_token(&url, o->token);
    const char *u = buf_cstr(&url);
    printf("%s\n", u);
    fflush(stdout);
    if (o->browser && !url.oom) {
        /* Two forks: the browser opener is not our child, so no zombie and no wait. */
        pid_t p = fork();
        if (p == 0) {
            if (fork() == 0) {
                setsid();
                int nul = open("/dev/null", O_RDWR);
                if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
                /* The listening socket and the rest are not the browser's: a browser started
                 * by xdg-open would keep the port after the host stopped. */
                for (int fd = 3; fd < 1024; fd++) close(fd);
                execlp(BROWSER_OPENER, BROWSER_OPENER, u, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (p > 0) waitpid(p, NULL, 0);
        else fprintf(stderr, "bimble-ccore: cannot start " BROWSER_OPENER ": open the address above\n");
    }
    buf_free(&url);
}

/* <exe dir>/web, from /proc/self/exe (macOS: _NSGetExecutablePath, links resolved; else argv[0]);
 * 0 when it does not fit. */
static int default_web(const char *argv0, char *out, size_t n) {
    char exe[PATH_MAX];
#ifdef __APPLE__
    char path[PATH_MAX];
    uint32_t pn = sizeof path;
    ssize_t k = -1;
    if (_NSGetExecutablePath(path, &pn) == 0 && realpath(path, exe)) k = (ssize_t)strlen(exe);
#else
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) exe[k] = 0;
#endif
    if (k <= 0) {
        if (strlen(argv0) >= sizeof exe) return 0;
        strcpy(exe, argv0);
    }
    char *slash = strrchr(exe, '/');
    int w = slash ? snprintf(out, n, "%.*s/web", (int)(slash - exe), exe) : snprintf(out, n, "web");
    return w > 0 && (size_t)w < n;
}

typedef struct {
    const char *model, *session;
    int want_session, warm;
    const cc_static *web;
    OpenCtx *open;
} Serve;

/* Opens the model (if any) and serves until SIGTERM or SIGINT. */
static int serve(unsigned port, Serve *sv) {
    const char *session = sv->session;
    int want_session = sv->want_session;
    /* The host's one session (a handle: the process could hold more). */
    cc_session *ses = cc_session_new(cc_platform_native());
    if (!ses) {
        fprintf(stderr, "bimble-ccore: out of memory\n");
        return 1;
    }
    const char *model = sv->model;
    if (model && *model) {
        cc_error err;
        static char def[4200];
        if (want_session && !session) {
            snprintf(def, sizeof def, "%s.ccore-session", model);
            session = def;
        }
        if (want_session ? cc_session_open_persisted(ses, model, session, &err) : cc_session_open(ses, model, &err)) {
            fprintf(stderr, "bimble-ccore: %s: %s\n", model, err.msg);
            return 1;
        }
    } else if (want_session) {
        fprintf(stderr, "bimble-ccore: --session needs a model\n");
        return 2;
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
    cfg.warm = sv->warm;
    cfg.session = ses;
    cfg.token = t;
    cfg.epoch = epoch;
    cfg.web = sv->web;
    if (sv->open) {
        sv->open->token = t;
        cfg.on_listen = on_listen;
        cfg.on_listen_ctx = sv->open;
    }
    cfg.origins[cfg.n_origins++] = "http://localhost:5173";
    cfg.origins[cfg.n_origins++] = "http://127.0.0.1:5173";
    static char extra[1024];
    const char *e = getenv("BIMBLE_ORIGINS");
    if (e && strlen(e) < sizeof extra) {
        strcpy(extra, e);
        for (char *tok = strtok(extra, ","); tok && cfg.n_origins < CC_HOST_MAX_ORIGINS; tok = strtok(NULL, ","))
            if (*tok && strlen(tok) < 256) cfg.origins[cfg.n_origins++] = tok;
    }
    int rc = cc_host_run(&cfg);
    cc_session_persist_now(ses); /* SIGTERM or SIGINT: the last change, if not written yet */
    cc_session_free(ses);
    return rc;
}

static const char OPEN_USAGE[] =
    "usage: bimble-ccore open [--port N] [--no-browser] [--web dir] [--session[=file]] [model]\n";

/* bimble-ccore open ... (see the header). argv[0] is "open". */
static int open_main(const char *argv0, int argc, char **argv) {
    Serve sv;
    memset(&sv, 0, sizeof sv);
    sv.warm = 1;
    OpenCtx oc = {NULL, 1};
    unsigned port = 0;
    const char *web = NULL;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--no-browser")) oc.browser = 0;
        else if (!strcmp(a, "--session")) sv.want_session = 1;
        else if (!strncmp(a, "--session=", 10) && a[10]) sv.want_session = 1, sv.session = a + 10;
        else if (!strcmp(a, "--port") && i + 1 < argc) {
            if (!parse_port(argv[++i], &port)) {
                fprintf(stderr, "bimble-ccore: \"%s\" is not a port\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--web") && i + 1 < argc) web = argv[++i];
        else if (!strcmp(a, "--help")) {
            fputs(OPEN_USAGE, stdout);
            return 0;
        } else {
            fprintf(stderr, "bimble-ccore: open: unknown flag %s\n%s", a, OPEN_USAGE);
            return 2;
        }
    }
    if (argc - i > 1) {
        fputs(OPEN_USAGE, stderr);
        return 2;
    }
    sv.model = i < argc ? argv[i] : NULL;
    static char defweb[PATH_MAX + 8];
    if (!web) {
        if (!default_web(argv0, defweb, sizeof defweb)) {
            fprintf(stderr, "bimble-ccore: cannot find the web/ folder next to the program: use --web <dir>\n");
            return 1;
        }
        web = defweb;
    }
    char err[600];
    cc_static *st = cc_static_load(web, err, sizeof err);
    if (!st) {
        fprintf(stderr, "bimble-ccore: the lab's files: %s\n", err);
        return 1;
    }
    size_t nf, nb;
    cc_static_stats(st, &nf, &nb);
    fprintf(stderr, "bimble-ccore: serving %zu files (%zu bytes) from %s\n", nf, nb, web);
    sv.web = st;
    sv.open = &oc;
    int rc = serve(port, &sv);
    cc_static_free(st);
    return rc;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "mcp")) return cc_mcp_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("bimble-ccore %s\n", CC_RELEASE_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "open")) return open_main(argv[0], argc - 1, argv + 1);
    /* Flags first, then the positional port and model. */
    Serve sv;
    memset(&sv, 0, sizeof sv);
    const char *session = getenv("CCORE_SESSION");
    sv.want_session = session && *session;
    sv.session = sv.want_session ? session : NULL;
    while (argc > 1 && !strncmp(argv[1], "--", 2)) {
        if (!strcmp(argv[1], "--session")) sv.want_session = 1;
        else if (!strcmp(argv[1], "--warm")) sv.warm = 1;
        else if (!strncmp(argv[1], "--session=", 10) && argv[1][10]) sv.want_session = 1, sv.session = argv[1] + 10;
        else {
            fprintf(stderr, "bimble-ccore: unknown flag %s\n", argv[1]);
            return 2;
        }
        argv[1] = argv[0], argv++, argc--;
    }
    const char *ps = argc > 1 ? argv[1] : getenv("BIMBLE_SERVE");
    unsigned port;
    if (!ps) {
        fprintf(stderr, "usage: bimble-ccore <port> [model] (or BIMBLE_SERVE=<port>, CCORE_MODEL=<model>)\n"
                        "       bimble-ccore open [--port N] [--no-browser] [model]\n");
        return 2;
    }
    if (!parse_port(ps, &port)) {
        fprintf(stderr, "bimble-ccore: \"%s\" is not a port\n", ps);
        return 2;
    }
    sv.model = argc > 2 ? argv[2] : getenv("CCORE_MODEL");
    return serve(port, &sv);
}
