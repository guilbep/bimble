/* The native host (L6 carrier): the HTTP server, the transport of
 * crates/bimble-viewer/src/serve.rs, single threaded over poll(), and the native platform. */
#ifndef CCORE_HOST_HOST_H
#define CCORE_HOST_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "session.h"
#include "static.h"

#define CC_HOST_MAX_ORIGINS 8

typedef struct {
    uint16_t port;      /* 0: any free port */
    const char *token;  /* the `t=` query parameter every request but OPTIONS must carry */
    const char *epoch;  /* per process, sent with each change (twin.rs:121, `random_token`) */
    const char *origins[CC_HOST_MAX_ORIGINS]; /* allowed `Origin` values */
    int n_origins;
    int warm;           /* when idle, build the scene and /mesh of a new rev before they are asked for */
    cc_session *session; /* the session the calls and /mesh serve (main.c's default session) */
    /* `bimble-ccore open` (ticket R1): the built lab, or NULL. When set, a GET of any path but
     * /change, /frame and /mesh is a file of this list (200) or 404, without the token (the page
     * and its assets are public; the calls still need it), and the host's own origins
     * (http://127.0.0.1:<port>, http://localhost:<port>) are allowed beside `origins`. */
    const cc_static *web;
    /* Called once the socket listens, with the bound port (open prints the URL then), or NULL. */
    void (*on_listen)(uint16_t port, void *ctx);
    void *on_listen_ctx;
} cc_host_config;

/* Binds 127.0.0.1:port, prints "bimble-ccore: listening <port> token <token>" on stderr,
 * publishes the first change and serves until SIGTERM or SIGINT. Returns 0 then, 1 when it
 * cannot listen. */
int cc_host_run(const cc_host_config *cfg);

/* The change feed (server.c): publishes `json` if it differs from the last one published,
 * bumps seq and answers every waiting GET /change (twin.rs `Twin::publish`). */
void cc_host_publish_change(const char *json, size_t n);

/* The native cc_platform (platform_native.c): POSIX files, CLOCK_MONOTONIC, time(), /dev/urandom,
 * stderr. Static, never freed. On Windows platform_win.c (ticket WIN1). */
const cc_platform *cc_platform_native(void);
/* LINKF: the native platform's path_allowed asks this rule (host/fs.c sets it at cc_host_fs_init);
 * NULL: every path. */
extern int (*cc_host_path_rule)(const char *path);

#ifdef _WIN32
#include <stdio.h>
/* platform_win.c: UTF-8 <-> UTF-16 (malloc'd, NULL on an invalid string or out of memory), and
 * fopen of a UTF-8 path. */
wchar_t *cc_win_wide(const char *s);
char *cc_win_utf8(const wchar_t *w);
FILE *cc_win_fopen(const char *path, const char *mode);
/* FIXDOC: getenv() gives the ANSI code page's bytes ("C:\Users\J?r?me"), not UTF-8: the variable
 * through _wgetenv and cc_win_utf8, kept for the process's life (the host never sets one). NULL
 * when unset or not convertible. */
const char *cc_win_getenv(const char *name);
#define cc_host_getenv(name) cc_win_getenv(name)
#else
#define cc_host_getenv(name) getenv(name)
#endif
/* FIXDOC: the user's Bimble folder, Rust's settings.rs `path` without the file name: macOS
 * $HOME/Library/Application Support/Bimble, Windows %APPDATA%\Bimble, else
 * $XDG_CONFIG_HOME/bimble or $HOME/.config/bimble ('/' separators added). 1 when written to
 * `buf`, 0 without the variables or when it does not fit (main.c). The settings file and the
 * optional fonts (fonts.c: <this>/fonts) live there, one spelling on every system. */
int cc_host_config_dir(char *buf, size_t n);
/* D7, SIZE1 (fonts.c): names the folder of the PDF export's optional Roboto files to session `s`:
 * $BIMBLE_FONTS, else the repository's ccore/fonts, else <the user's Bimble folder>/fonts.
 * `web` is not used (no font is shipped). */
void cc_host_fonts(cc_session *s, const cc_static *web);
#endif
