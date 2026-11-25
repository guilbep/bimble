/* The native host's transport: serve.rs rewritten as one thread over poll(), so a long-polling
 * GET /change never blocks the page's synchronous POST calls (spec §6, last risk).
 * Same limits, checks, status codes and headers as serve.rs; each is cited. Input is untrusted:
 * bounded buffers, no recursion, every parse error is an answer or a closed connection. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* macOS hides SO_NOSIGPIPE and MSG_DONTWAIT under _POSIX_C_SOURCE alone */
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "calls.h"
#include "host.h"
#include "mesh.h"
#include "session.h"

#define MAX_BODY (1u << 20)        /* serve.rs:14 */
#define MAX_HEADER (16u << 10)     /* serve.rs:15 */
#define MAX_CONNECTIONS 32         /* serve.rs:16 */
#define IDLE_MS 30000              /* serve.rs:17, the read timeout */
#define POLL_TIMEOUT_MS 25000      /* serve.rs:18, the long-poll timeout */
#define FRAME_MIN_W 16             /* serve.rs:19 */
#define FRAME_MAX 4096             /* serve.rs:20 */
/* After a "Connection: close" answer, read and drop what the client still sends (at most this
 * much, this long) before close(): closing with unread bytes resets the connection and the
 * client may lose the answer (a 413 sent before a 1 MiB body was read). */
#define DRAIN_MS 1000
#define DRAIN_MAX (4u << 20)

/* A write to a closed peer: EPIPE, never SIGPIPE (also ignored in cc_host_run). macOS has no
 * MSG_NOSIGNAL: SO_NOSIGPIPE on each accepted socket instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static const char TEXT[] = "text/plain; charset=utf-8";

enum { C_FREE = 0, C_READ, C_WAIT, C_WRITE, C_DRAIN };

typedef struct {
    int fd;
    int st;
    Buf in;          /* bytes read, not yet consumed by a request */
    Buf out;         /* the answer being written */
    size_t out_off;
    int keep;        /* after the answer: 1 read the next request, 0 close */
    long long deadline; /* C_READ: idle; C_WAIT: long-poll end; C_DRAIN: give up */
    uint64_t since;  /* C_WAIT */
    char acao[256];  /* the allowed Origin of the waiting request, "" for none */
    size_t drained;
    int pending;     /* C_READ with bytes already read: parse them without waiting for input */
} Conn;

static Conn conns[MAX_CONNECTIONS];
static const cc_host_config *cfg;
static uint16_t bound_port;
static volatile sig_atomic_t stop;

/* The change feed (twin.rs:115-118, `Twin::publish` twin.rs:160-171): seq, the last change. */
static uint64_t seq;
static Buf change;   /* "null" until the first publish */
static int has_last;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

/* ---- answers ---- */

static const char *reason(int status) { /* serve.rs:306-316 */
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "Service Unavailable";
    }
}

static void conn_close(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    buf_free(&c->in);
    buf_free(&c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->st = C_FREE;
}

static void start_drain(Conn *c) {
    shutdown(c->fd, SHUT_WR);
    c->st = C_DRAIN;
    c->drained = 0;
    c->deadline = now_ms() + DRAIN_MS;
}

/* Writes what is pending; then reads the next request, or drains and closes. */
static void flush_out(Conn *c) {
    while (c->out_off < c->out.len) {
        ssize_t k = send(c->fd, c->out.p + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
        if (k > 0) {
            c->out_off += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            c->st = C_WRITE;
            return;
        }
        conn_close(c);
        return;
    }
    buf_clear(&c->out);
    c->out_off = 0;
    if (!c->keep) {
        start_drain(c);
        return;
    }
    c->st = C_READ;
    c->deadline = now_ms() + IDLE_MS;
    c->pending = c->in.len > 0; /* a pipelined request already read: the main loop serves it */
}

/* serve.rs:296-327 `respond`: the same header lines in the same order. */
/* `extra`: more header lines ("Name: value\r\n"), or NULL (GET /mesh sends X-Rev). */
static void respond_x(Conn *c, int status, const char *ctype, const char *extra, const char *body, size_t n,
                      int keep) {
    Buf *o = &c->out;
    buf_clear(o);
    c->out_off = 0;
    buf_puts(o, "HTTP/1.1 ");
    buf_u64(o, (uint64_t)status);
    buf_putc(o, ' ');
    buf_puts(o, reason(status));
    buf_puts(o, "\r\n");
    if (c->acao[0]) {
        buf_puts(o, "Access-Control-Allow-Origin: ");
        buf_puts(o, c->acao);
        buf_puts(o, "\r\nVary: Origin\r\n");
    }
    if (status == 204)
        buf_puts(o, "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Access-Control-Max-Age: 600\r\n");
    if (extra) buf_puts(o, extra);
    buf_puts(o, "Content-Type: ");
    buf_puts(o, ctype);
    buf_puts(o, "\r\nContent-Length: ");
    buf_u64(o, (uint64_t)n);
    buf_puts(o, keep ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n");
    buf_append(o, body, n);
    if (o->oom) { /* no answer can be built: drop the connection */
        conn_close(c);
        return;
    }
    c->keep = keep;
    flush_out(c);
}

static void respond(Conn *c, int status, const char *ctype, const char *body, size_t n, int keep) {
    respond_x(c, status, ctype, NULL, body, n, keep);
}

static void respond_s(Conn *c, int status, const char *body, int keep) {
    respond(c, status, TEXT, body, strlen(body), keep);
}

/* serve.rs:247-251: `{"epoch":"<e>","seq":<n>,"change":<json>}` (a format! string, this order). */
static void respond_change(Conn *c, uint64_t s, const char *json, size_t n) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"epoch\":\"");
    buf_puts(&b, cfg->epoch);
    buf_puts(&b, "\",\"seq\":");
    buf_u64(&b, s);
    buf_puts(&b, ",\"change\":");
    buf_append(&b, json, n);
    buf_putc(&b, '}');
    if (b.oom) conn_close(c);
    else respond(c, 200, "application/json", b.p, b.len, c->keep);
    buf_free(&b);
}

/* ---- the change feed ---- */

void cc_host_publish_change(const char *json, size_t n) {
    if (has_last && change.len == n && memcmp(change.p, json, n) == 0) return;
    buf_clear(&change);
    buf_append(&change, json, n);
    has_last = 1;
    seq++;
    /* Wake every long-poll: seq > since for each (twin.rs:170, notify_all). */
    for (int i = 0; i < MAX_CONNECTIONS; i++)
        if (conns[i].st == C_WAIT && seq > conns[i].since) respond_change(&conns[i], seq, change.p, change.len);
}

/* ---- request parsing (serve.rs:96-182 `read_head`) ---- */

typedef struct {
    const char *p;
    size_t n;
} Str;

typedef struct {
    Str method, target, origin, host;
    int has_origin, has_host, has_len, close;
    size_t content_length;
    size_t head_len; /* bytes up to and including the blank line */
} Head;

enum { H_OK, H_MORE, H_TOO_LARGE, H_MALFORMED };

static int str_eq(Str s, const char *z) { return s.n == strlen(z) && memcmp(s.p, z, s.n) == 0; }

static Str trim(Str s) { /* Rust str::trim for ASCII whitespace (the only kind HTTP uses) */
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t' || s.p[0] == '\r' || s.p[0] == '\n')) s.p++, s.n--;
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r' || s.p[s.n - 1] == '\n')) s.n--;
    return s;
}

static int ieq(Str s, const char *z) {
    size_t n = strlen(z);
    if (s.n != n) return 0;
    for (size_t i = 0; i < n; i++) {
        char a = s.p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != z[i]) return 0;
    }
    return 1;
}

static int icontains(Str s, const char *z) {
    size_t n = strlen(z);
    for (size_t i = 0; i + n <= s.n; i++)
        if (ieq((Str){s.p + i, n}, z)) return 1;
    return 0;
}

static int utf8_ok(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        size_t k;
        unsigned min;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) k = 1, min = 0x80;
        else if (c >= 0xE0 && c <= 0xEF) k = 2, min = 0x800;
        else if (c >= 0xF0 && c <= 0xF4) k = 3, min = 0x10000;
        else return 0;
        if (i + k >= n) return 0;
        unsigned v = c & (0x3F >> k);
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            v = (v << 6) | (s[i + j] & 0x3F);
        }
        if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

/* Parses a decimal usize as Rust's `parse::<usize>` (an optional '+', digits, no overflow). */
static int parse_u64(Str s, uint64_t *out) {
    size_t i = 0;
    if (s.n && s.p[0] == '+') i = 1;
    if (i == s.n) return 0;
    uint64_t v = 0;
    for (; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') return 0;
        unsigned d = (unsigned)(s.p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* The request line: exactly three parts split on ' ' (serve.rs:137-145); sets the version's
 * default for close (HTTP/1.0 closes, serve.rs:152), which a Connection header then overrides. */
static int request_line(Str first, Head *h) {
    Str parts[3];
    size_t np = 0, s = 0;
    for (size_t j = 0; j <= first.n; j++) {
        if (j == first.n || first.p[j] == ' ') {
            if (np == 3) return 0;
            parts[np++] = (Str){first.p + s, j - s};
            s = j + 1;
        }
    }
    if (np != 3) return 0;
    Str ver = parts[2];
    if (ver.n < 7 || memcmp(ver.p, "HTTP/1.", 7) != 0 || parts[0].n == 0 || parts[1].n == 0 || parts[1].p[0] != '/')
        return 0;
    h->method = parts[0];
    h->target = parts[1];
    h->close = str_eq(ver, "HTTP/1.0");
    return 1;
}

/* One header block at the start of p[0..len): lines up to a blank line, at most MAX_HEADER
 * bytes, each valid UTF-8 (serve.rs:96-182 `read_head`). */
static int parse_head(const char *p, size_t len, Head *h) {
    memset(h, 0, sizeof *h);
    size_t i = 0, lines = 0;
    size_t limit = len < MAX_HEADER ? len : MAX_HEADER;
    for (;;) {
        const char *nl = i < limit ? memchr(p + i, '\n', limit - i) : NULL;
        if (!nl) return len > MAX_HEADER ? H_TOO_LARGE : H_MORE;
        size_t end = (size_t)(nl - p) + 1;
        Str line = {p + i, end - i};
        if (!utf8_ok((const unsigned char *)line.p, line.n)) return H_MALFORMED;
        /* trim_end_matches(['\r', '\n']) */
        while (line.n && (line.p[line.n - 1] == '\r' || line.p[line.n - 1] == '\n')) line.n--;
        i = end;
        if (line.n == 0) {
            if (lines == 0) continue; /* a stray CRLF between requests */
            break;
        }
        if (lines == 0) {
            if (!request_line(line, h)) return H_MALFORMED;
        } else {
            const char *colon = memchr(line.p, ':', line.n);
            if (!colon) return H_MALFORMED;
            Str k = trim((Str){line.p, (size_t)(colon - line.p)});
            Str v = trim((Str){colon + 1, line.n - (size_t)(colon - line.p) - 1});
            if (ieq(k, "origin")) h->origin = v, h->has_origin = 1;
            else if (ieq(k, "host")) h->host = v, h->has_host = 1;
            else if (ieq(k, "content-length")) {
                uint64_t n;
                if (!parse_u64(v, &n)) return H_MALFORMED;
                if (h->has_len && h->content_length != n) return H_MALFORMED;
                h->content_length = (size_t)n;
                h->has_len = 1;
            } else if (ieq(k, "transfer-encoding")) return H_MALFORMED;
            else if (ieq(k, "connection")) {
                if (icontains(v, "close")) h->close = 1;
                else if (icontains(v, "keep-alive")) h->close = 0;
            }
        }
        lines++;
    }
    h->head_len = i;
    return H_OK;
}

/* serve.rs:368-374: the first `key=value` pair with this key; no decoding. */
static int query_param(Str q, const char *key, Str *out) {
    size_t kn = strlen(key), i = 0;
    while (i <= q.n) {
        const char *amp = memchr(q.p + i, '&', q.n - i);
        size_t end = amp ? (size_t)(amp - q.p) : q.n;
        Str kv = {q.p + i, end - i};
        const char *eq = memchr(kv.p, '=', kv.n);
        if (eq && (size_t)(eq - kv.p) == kn && memcmp(kv.p, key, kn) == 0) {
            *out = (Str){eq + 1, kv.n - kn - 1};
            return 1;
        }
        if (!amp) break;
        i = end + 1;
    }
    return 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* serve.rs:377-394; 0 on a bad escape, invalid UTF-8 or a NUL (a C string cannot hold it). */
static int percent_decode(Str s, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '%') {
            if (i + 2 >= s.n) return 0;
            int a = hexv(s.p[i + 1]), b = hexv(s.p[i + 2]);
            if (a < 0 || b < 0) return 0;
            buf_putc(out, (char)(a * 16 + b));
            i += 2;
        } else {
            buf_putc(out, s.p[i]);
        }
    }
    if (out->oom || memchr(out->p ? out->p : "", 0, out->len) || !utf8_ok((const unsigned char *)out->p, out->len)) return 0;
    buf_cstr(out);
    return 1;
}

/* ---- routing ---- */

static void route_post(Conn *c, Str path, const char *body, size_t n, int keep) {
    /* serve.rs:268-284: two segments after the leading slashes, else 404. */
    Str rest = path;
    while (rest.n && rest.p[0] == '/') rest.p++, rest.n--;
    const char *slash = memchr(rest.p, '/', rest.n);
    if (!slash || memchr(slash + 1, '/', rest.n - (size_t)(slash - rest.p) - 1)) {
        respond_s(c, 404, "not found", keep);
        return;
    }
    Str op_s = {rest.p, (size_t)(slash - rest.p)};
    Str name_s = {slash + 1, rest.n - op_s.n - 1};
    Buf op, name, b, out;
    buf_init(&op), buf_init(&name), buf_init(&b), buf_init(&out);
    int ok = percent_decode(op_s, &op);
    if (ok && str_eq(name_s, "-")) buf_cstr(&name);
    else if (ok) ok = percent_decode(name_s, &name);
    if (ok && !utf8_ok((const unsigned char *)body, n)) ok = 0;
    if (!ok) {
        respond_s(c, 400, "malformed request", 0);
        goto done;
    }
    buf_append(&b, body, n);
    const char *bz = buf_cstr(&b);
    const char *opz = buf_cstr(&op), *namez = buf_cstr(&name);
    int status;
    if (!cc_known_op(opz)) { /* calls.rs:42 */
        buf_puts(&out, "unknown op ");
        buf_puts(&out, opz);
        status = CC_ERR;
    } else {
        status = cc_call(cfg->session, opz, namez, bz, n, &out);
        cc_session_persist_now(cfg->session); /* the session file has the change before the call answers */
    }
    if (out.oom) status = 500, buf_clear(&out), out.oom = 0;
    /* The twin publishes the change after serving each call (twin.rs `run_headless`); the
     * feed drops it when nothing changed. */
    Buf ch;
    buf_init(&ch);
    cc_change_json(cfg->session, &ch);
    if (!ch.oom) cc_host_publish_change(ch.p ? ch.p : "", ch.len);
    buf_free(&ch);
    respond(c, status == CC_OK ? 200 : status == CC_ERR ? 400 : 500, TEXT, out.p ? out.p : "", out.len, keep);
done:
    buf_free(&op), buf_free(&name), buf_free(&b), buf_free(&out);
}

static int origin_allowed(Str o) {
    for (int i = 0; i < cfg->n_origins; i++)
        if (str_eq(o, cfg->origins[i])) return 1;
    if (cfg->web) { /* the page this host serves itself (host.h `web`) */
        char self1[40], self2[40];
        snprintf(self1, sizeof self1, "http://127.0.0.1:%u", (unsigned)bound_port);
        snprintf(self2, sizeof self2, "http://localhost:%u", (unsigned)bound_port);
        if (str_eq(o, self1) || str_eq(o, self2)) return 1;
    }
    return 0;
}

/* One request if complete in c->in (serve.rs:184-293 `serve_connection`). */
static void handle_one(Conn *c, int *again) {
    *again = 0;
    Head h;
    int r = parse_head(c->in.p, c->in.len, &h);
    c->acao[0] = 0;
    if (r == H_MORE) return;
    if (r == H_TOO_LARGE) { respond_s(c, 431, "header block too large", 0); return; }
    if (r == H_MALFORMED) { respond_s(c, 400, "malformed request", 0); return; }

    int origin_ok = !h.has_origin || origin_allowed(h.origin);
    if (origin_ok && h.has_origin && h.origin.n < sizeof c->acao) {
        memcpy(c->acao, h.origin.p, h.origin.n);
        c->acao[h.origin.n] = 0;
    }
    const char *qm = memchr(h.target.p, '?', h.target.n);
    Str path = {h.target.p, qm ? (size_t)(qm - h.target.p) : h.target.n};
    Str query = qm ? (Str){qm + 1, h.target.n - path.n - 1} : (Str){"", 0};
    Str t;
    int token_ok = query_param(query, "t", &t) && str_eq(t, cfg->token);
    char want1[32], want2[32];
    snprintf(want1, sizeof want1, "127.0.0.1:%u", (unsigned)bound_port);
    snprintf(want2, sizeof want2, "localhost:%u", (unsigned)bound_port);
    int host_ok = h.has_host && (str_eq(h.host, want1) || str_eq(h.host, want2));
    int is_options = str_eq(h.method, "OPTIONS");
    /* The built lab (host.h `web`): a GET of a path that is not the API needs no token. */
    int is_static = cfg->web && str_eq(h.method, "GET") && !str_eq(path, "/change") && !str_eq(path, "/frame") &&
                    !str_eq(path, "/mesh");
    if (!origin_ok || !host_ok || (!token_ok && !is_options && !is_static)) {
        respond_s(c, 403, "forbidden", 0);
        return;
    }
    size_t len = h.has_len ? h.content_length : 0;
    if (len > MAX_BODY) {
        respond_s(c, 413, "body too large", 0);
        return;
    }
    if (c->in.len - h.head_len < len) return; /* the body is not all here yet */
    /* Consume the request now; keep a copy of the body for the call. */
    Buf body;
    buf_init(&body);
    buf_append(&body, c->in.p + h.head_len, len);
    Buf tgt;
    buf_init(&tgt);
    buf_append(&tgt, h.target.p, h.target.n);
    Str method = h.method;
    char m[8] = {0};
    if (method.n < sizeof m) memcpy(m, method.p, method.n);
    size_t used = h.head_len + len;
    int keep = !h.close;
    if (body.oom || tgt.oom) {
        buf_free(&body), buf_free(&tgt);
        conn_close(c);
        return;
    }
    memmove(c->in.p, c->in.p + used, c->in.len - used);
    c->in.len -= used;
    /* path and query now point into tgt */
    qm = memchr(tgt.p, '?', tgt.len);
    path = (Str){tgt.p, qm ? (size_t)(qm - tgt.p) : tgt.len};
    query = qm ? (Str){qm + 1, tgt.len - path.n - 1} : (Str){"", 0};

    if (is_options) {
        respond_s(c, 204, "", keep);
    } else if (!strcmp(m, "GET") && str_eq(path, "/change")) {
        Str s;
        uint64_t since;
        if (!query_param(query, "since", &s) || !parse_u64(s, &since)) {
            respond_s(c, 400, "bad since", 0);
        } else {
            Str e;
            int stale = query_param(query, "epoch", &e) && !str_eq(e, cfg->epoch);
            c->keep = keep;
            /* twin.rs:55-73 `wait_change`; stale = wait_change(0, ZERO) (serve.rs:238-246). */
            if (stale) {
                if (seq > 0) respond_change(c, seq, change.p, change.len);
                else respond_change(c, seq, "null", 4);
            } else if (since > seq || seq > since) {
                respond_change(c, seq, change.p, change.len);
            } else {
                c->st = C_WAIT;
                c->since = since;
                c->deadline = now_ms() + POLL_TIMEOUT_MS;
            }
        }
    } else if (!strcmp(m, "GET") && str_eq(path, "/frame")) {
        /* serve.rs:256-267: w in 16..=4096, h in 1..=4096; then the default handler: no frames. */
        Str w, hh;
        uint64_t wv = 0, hv = 0;
        int ok = query_param(query, "w", &w) && parse_u64(w, &wv) && wv >= FRAME_MIN_W && wv <= FRAME_MAX &&
                 query_param(query, "h", &hh) && parse_u64(hh, &hv) && hv >= 1 && hv <= FRAME_MAX;
        if (!ok) respond_s(c, 400, "bad frame size", keep);
        else respond_s(c, 500, "no frames", keep); /* serve.rs:39-41 */
    } else if (!strcmp(m, "GET") && str_eq(path, "/mesh")) {
        /* The scene as BMSH for the WebGL2 viewport (mesh.c, spec §2). */
        const char *mp;
        size_t mn;
        uint32_t rev;
        double ms;
        int st = cc_mesh_get(cfg->session, &mp, &mn, &rev, &ms);
        if (ms > 0) fprintf(stderr, "bimble-ccore: mesh rev %u built in %.3f ms, %zu bytes\n", (unsigned)rev, ms, mn);
        char xrev[80];
        snprintf(xrev, sizeof xrev, "X-Rev: %u\r\nAccess-Control-Expose-Headers: X-Rev\r\n", (unsigned)rev);
        respond_x(c, st, st == 200 ? "application/octet-stream" : TEXT, st == 200 ? xrev : NULL, mp, mn, keep);
    } else if (is_static) {
        const cc_static_file *f = cc_static_find(cfg->web, path.p, path.n);
        if (f)
            respond_x(c, 200, f->ctype, "X-Content-Type-Options: nosniff\r\nCache-Control: no-cache\r\n", f->data,
                      f->n, keep);
        else respond_s(c, 404, "not found", keep);
    } else if (!strcmp(m, "POST")) {
        route_post(c, path, body.p ? body.p : "", body.len, keep);
    } else {
        respond_s(c, 404, "not found", keep);
    }
    buf_free(&body);
    buf_free(&tgt);
    *again = 1;
}

static void handle_input(Conn *c) {
    int again = 1;
    c->pending = 0;
    while (again && c->st == C_READ && c->in.len) handle_one(c, &again);
}

/* Reads what is there. Returns 0 when the connection was closed. */
static int read_some(Conn *c) {
    char tmp[65536];
    for (;;) {
        /* Never hold more than one request's worth (the head limit plus the body limit): only a
         * client pipelining megabytes behind a long-poll gets there; drop it. */
        if (c->in.len > MAX_HEADER + MAX_BODY && c->st != C_DRAIN) {
            conn_close(c);
            return 0;
        }
        ssize_t k = recv(c->fd, tmp, sizeof tmp, 0);
        if (k > 0) {
            if (c->st == C_DRAIN) {
                c->drained += (size_t)k;
                if (c->drained > DRAIN_MAX) { conn_close(c); return 0; }
                continue;
            }
            buf_append(&c->in, tmp, (size_t)k);
            if (c->in.oom) { conn_close(c); return 0; }
            c->deadline = c->st == C_READ ? now_ms() + IDLE_MS : c->deadline;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        /* EOF or error. A partial request at EOF is malformed (serve.rs:113-118). */
        if (k == 0 && c->st == C_READ && c->in.len) {
            Head h;
            int r = parse_head(c->in.p, c->in.len, &h);
            if (r != H_OK) {
                respond_s(c, 400, "malformed request", 0);
                return c->st != C_FREE;
            }
        }
        conn_close(c);
        return 0;
    }
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int cc_host_run(const cc_host_config *config) {
    cfg = config;
    for (int i = 0; i < MAX_CONNECTIONS; i++) conns[i].fd = -1;
    buf_init(&change);
    buf_puts(&change, "null"); /* twin.rs:120: the feed starts at (0, "null") */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("bimble-ccore: socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg->port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 64) < 0 || set_nonblock(lfd) < 0) {
        fprintf(stderr, "bimble-ccore: cannot listen on %u: %s\n", (unsigned)cfg->port, strerror(errno));
        close(lfd);
        return 1;
    }
    socklen_t al = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &al);
    bound_port = ntohs(a.sin_port);

    Buf ch;
    buf_init(&ch);
    cc_change_json(cfg->session, &ch);
    cc_host_publish_change(ch.p ? ch.p : "null", ch.len);
    buf_free(&ch);

    fprintf(stderr, "bimble-ccore: listening %u token %s\n", (unsigned)bound_port, cfg->token);
    fflush(stderr);
    if (cfg->on_listen) cfg->on_listen(bound_port, cfg->on_listen_ctx);

    struct pollfd pfd[MAX_CONNECTIONS + 1];
    int idx[MAX_CONNECTIONS + 1];
    while (!stop) {
        long long now = now_ms();
        int timeout = 1000;
        int n = 0;
        pfd[n].fd = lfd, pfd[n].events = POLLIN, pfd[n].revents = 0, idx[n] = -1, n++;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            Conn *c = &conns[i];
            if (c->st == C_FREE) continue;
            short ev = c->st == C_WRITE ? POLLOUT : POLLIN;
            pfd[n].fd = c->fd, pfd[n].events = ev, pfd[n].revents = 0, idx[n] = i, n++;
            long long left = c->deadline - now;
            if (c->st != C_WRITE && left < timeout) timeout = left < 0 ? 0 : (int)left;
        }
        /* --warm: a new rev (load, a change) and nothing to read: build its scene and BMSH now,
         * while the page is still reacting to the change, so /mesh finds them cached. */
        static uint32_t warmed;
        int want_warm = cfg->warm && cc_session_model(cfg->session) && cc_session_rev(cfg->session) != warmed;
        if (want_warm) timeout = 0;
        int r = poll(pfd, (nfds_t)n, timeout);
        if (want_warm && r == 0) {
            const char *mp;
            size_t mn;
            uint32_t rev;
            double ms;
            cc_mesh_get(cfg->session, &mp, &mn, &rev, &ms);
            warmed = rev;
            fprintf(stderr, "bimble-ccore: warm: mesh rev %u built in %.3f ms (scene included)\n", (unsigned)rev, ms);
            continue;
        }
        if (r < 0 && errno != EINTR) { perror("bimble-ccore: poll"); break; }
        if (r > 0) {
            if (pfd[0].revents & POLLIN) {
                for (;;) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd < 0) break;
#ifdef SO_NOSIGPIPE
                    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                    int slot = -1;
                    for (int i = 0; i < MAX_CONNECTIONS; i++)
                        if (conns[i].st == C_FREE) { slot = i; break; }
                    if (slot < 0) { /* serve.rs:59-70 */
                        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain; "
                                                   "charset=utf-8\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbusy";
                        ssize_t k = send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
                        (void)k;
                        close(fd);
                        continue;
                    }
                    set_nonblock(fd);
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Conn *c = &conns[slot];
                    memset(c, 0, sizeof *c);
                    c->fd = fd;
                    c->st = C_READ;
                    c->deadline = now_ms() + IDLE_MS;
                    buf_init(&c->in);
                    buf_init(&c->out);
                }
            }
            for (int j = 1; j < n; j++) {
                Conn *c = &conns[idx[j]];
                if (!pfd[j].revents || c->st == C_FREE || c->fd != pfd[j].fd) continue;
                if (c->st == C_WRITE) {
                    if (pfd[j].revents & (POLLOUT | POLLERR | POLLHUP)) flush_out(c);
                    continue;
                }
                int st = c->st;
                if (!read_some(c)) continue;
                if (st == C_READ && c->st == C_READ) handle_input(c);
            }
        }
        for (int i = 0; i < MAX_CONNECTIONS; i++)
            if (conns[i].st == C_READ && conns[i].pending) handle_input(&conns[i]);
        now = now_ms();
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            Conn *c = &conns[i];
            if (c->st == C_FREE || c->st == C_WRITE || c->deadline > now) continue;
            if (c->st == C_WAIT) {
                /* Timed out: seq <= since, so change is null (twin.rs:66-70). */
                respond_change(c, seq, "null", 4);
            } else {
                conn_close(c); /* idle read, or a drain that took too long */
            }
        }
    }
    for (int i = 0; i < MAX_CONNECTIONS; i++)
        if (conns[i].st != C_FREE) conn_close(&conns[i]);
    close(lfd);
    buf_free(&change);
    fprintf(stderr, "bimble-ccore: stopped\n");
    return 0;
}
