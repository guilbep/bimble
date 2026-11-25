/* The exported wasm32 API (see wasm_api.h for the protocol). Compiled only for __wasm__: the
 * native and tcc builds see an empty file. */
#ifdef __wasm__
#include "wasm_api.h"
#include <stdlib.h>
#include <string.h>
#include "buf.h"
#include "command.h"
#include "file.h"
#include "fmt.h"
#include "guid.h"
#include "history.h"
#include "ifc_in.h"
#include "model.h"
#include "query.h"
#include "scene.h"
#include "strtod.h"
#include "verbs.h"
#include "calls.h"   /* host/, -Ihost */
#include "mesh.h"
#include "session.h"

#define EXPORT(name) __attribute__((export_name(name)))

typedef struct {
    int used;
    uint32_t rev;
    cc_model m;
    cc_history h;
} Doc;
static Doc docs[CC_WASM_MAX_MODELS];

size_t cc_wasm_heap_used(void); /* wasm_stub.c */

/* Hands the buffer to the host: res = (ptr, len). The host frees ptr. */
static int answer(cc_wasm_res *r, Buf *b, int status) {
    if (b->oom) {
        buf_free(b);
        buf_init(b);
        buf_puts(b, "out of memory");
        status = 500;
        if (b->oom) {
            buf_free(b);
            r->ptr = r->len = 0;
            return 500;
        }
    }
    r->ptr = b->len ? (uint32_t)(uintptr_t)b->p : 0;
    r->len = (uint32_t)b->len;
    if (!b->len) buf_free(b);
    return status;
}

static int text(cc_wasm_res *r, int status, const char *msg) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, msg);
    return answer(r, &b, status);
}

static Doc *doc(int h, cc_wasm_res *r) {
    if (h < 1 || h > CC_WASM_MAX_MODELS || !docs[h - 1].used) {
        if (r) r->value = 0, text(r, 404, "no model with this handle");
        return NULL;
    }
    return &docs[h - 1];
}

static int new_doc(void) {
    for (int i = 0; i < CC_WASM_MAX_MODELS; i++)
        if (!docs[i].used) return i + 1;
    return 0;
}

EXPORT("cc_alloc") void *wa_alloc(size_t n) { return malloc(n ? n : 1); }
EXPORT("cc_free") void wa_free(void *p) { free(p); }
EXPORT("cc_heap_used") size_t wa_heap_used(void) { return cc_wasm_heap_used(); }

EXPORT("open_text") int wa_open_text(const char *p, size_t n, cc_wasm_res *r) {
    r->value = 0;
    int h = new_doc();
    if (!h) return text(r, 400, "too many open models");
    Doc *d = &docs[h - 1];
    cc_error err;
    if (cc_model_load(&d->m, p, n, &err)) {
        cc_model_free(&d->m);
        return text(r, 400, err.msg);
    }
    cc_history_init(&d->h);
    d->used = 1, d->rev = 0;
    r->value = (uint32_t)h;
    return text(r, 200, "");
}

EXPORT("open_ifc") int wa_open_ifc(const char *p, size_t n, cc_wasm_res *r) {
    r->value = 0;
    int h = new_doc();
    if (!h) return text(r, 400, "too many open models");
    Doc *d = &docs[h - 1];
    cc_error err;
    cc_ifc_report rep;
    if (cc_ifc_import(&d->m, &rep, p, n, &err)) {
        cc_ifc_report_free(&rep);
        cc_model_free(&d->m);
        return text(r, 400, err.msg);
    }
    Buf b;
    buf_init(&b);
    buf_append(&b, rep.lines.p, rep.lines.len);
    cc_ifc_report_free(&rep);
    cc_history_init(&d->h);
    d->used = 1, d->rev = 0;
    r->value = (uint32_t)h;
    return answer(r, &b, 200);
}

EXPORT("close") int wa_close(int h) {
    Doc *d = doc(h, NULL);
    if (!d) return 404;
    cc_history_free(&d->h);
    cc_model_free(&d->m);
    memset(d, 0, sizeof *d);
    return 200;
}

EXPORT("query") int wa_query(int h, const char *name, size_t nn, const char *args, size_t an, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = 0;
    char z[64];
    if (nn >= sizeof z) nn = sizeof z - 1; /* no query name is that long: still "unknown query" */
    memcpy(z, name, nn);
    z[nn] = 0;
    Buf b;
    buf_init(&b);
    int st = cc_query(&d->m, z, args, an, &b);
    return answer(r, &b, st);
}

EXPORT("apply") int wa_apply(int h, const char *p, size_t n, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = 0;
    cc_cmds l;
    cc_cmds_init(&l);
    Buf err;
    buf_init(&err);
    int st = 200;
    if (cc_commands_parse(p, n, &l, &err) || cc_history_apply(&d->h, &d->m, l.v, l.n, &err)) st = 400;
    else d->rev++;
    cc_cmds_free(&l);
    if (st == 200) buf_clear(&err);
    return answer(r, &err, st);
}

EXPORT("verb") int wa_verb(int h, const char *name, size_t nn, const char *args, size_t an, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = 0;
    char z[64];
    if (nn >= sizeof z) nn = sizeof z - 1;
    memcpy(z, name, nn);
    z[nn] = 0;
    cc_cmds l;
    cc_cmds_init(&l);
    Buf out, err;
    buf_init(&out), buf_init(&err);
    int st = 200;
    if (cc_verb_run(&d->m, z, args, an, &l, &err)) st = 400;
    else {
        cc_commands_json(l.v, l.n, &out);
        if (cc_history_apply(&d->h, &d->m, l.v, l.n, &err)) st = 400;
        else d->rev++;
    }
    cc_cmds_free(&l);
    if (st == 200) {
        buf_free(&err);
        return answer(r, &out, 200);
    }
    buf_free(&out);
    return answer(r, &err, st);
}

static int undo_redo(int h, cc_wasm_res *r, int redo) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    int did = redo ? cc_history_redo(&d->h, &d->m) : cc_history_undo(&d->h, &d->m);
    if (did) d->rev++;
    r->value = (uint32_t)did;
    return text(r, 200, did ? "true" : "false");
}
EXPORT("undo") int wa_undo(int h, cc_wasm_res *r) { return undo_redo(h, r, 0); }
EXPORT("redo") int wa_redo(int h, cc_wasm_res *r) { return undo_redo(h, r, 1); }

EXPORT("history") int wa_history(int h, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = 0;
    Buf b;
    buf_init(&b);
    cc_history_json(&d->h, &b);
    return answer(r, &b, 200);
}

EXPORT("save") int wa_save(int h, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = 0;
    Buf b;
    buf_init(&b);
    if (cc_model_save(&d->m, &b)) {
        buf_free(&b);
        return text(r, 500, "the model cannot be saved (a non-finite value or out of memory)");
    }
    return answer(r, &b, 200);
}

/* ---- mesh: host/mesh.c's BMSH walk, reimplemented on cc_scene_* ---- */

static void put_u32(Buf *b, uint32_t v) { buf_append(b, &v, 4); }

static int bmsh(const cc_model *m, uint32_t rev, int local, Buf *out, cc_error *err) {
    cc_scene s;
    int rc = -1;
    Arena tmp;
    arena_init(&tmp, 0);
    if (cc_scene_build(&s, m, err) != 0) goto done;
    size_t ne = s.nelements;
    cc_mesh *w = ne ? arena_alloc(&tmp, ne * sizeof *w, 16) : NULL;
    if (ne && !w) goto oom;
    uint64_t nv = 0, ni = 0, ns = 0;
    for (size_t k = 0; k < ne; k++) {
        if (local) w[k] = *cc_scene_local_mesh(&s, k);
        else if (cc_scene_world_mesh(&s, k, &tmp, &w[k]) != 0) goto oom;
        nv += w[k].nv, ni += w[k].ni, ns += w[k].ne;
    }
    if (ne > UINT32_MAX || nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) {
        err->offset = CC_NO_OFFSET;
        memcpy(err->msg, "scene too large for BMSH", 25);
        goto done;
    }
    if (buf_reserve(out, 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns + (local ? 52 * ne : 0)) != 0) goto oom;
    buf_append(out, "BMSH", 4);
    put_u32(out, local ? CC_BMSH_LOCAL_VERSION : 1u);
    put_u32(out, rev);
    put_u32(out, (uint32_t)ne), put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns);
    buf_append(out, s.origin, 24);
    uint32_t fv = 0, fi = 0, fs = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s.elements[k];
        buf_append(out, e->id.c, 22);
        buf_putc(out, (char)(unsigned char)e->category);
        buf_putc(out, 0);
        put_u32(out, fv), put_u32(out, w[k].nv), put_u32(out, fi), put_u32(out, w[k].ni);
        put_u32(out, fs), put_u32(out, w[k].ne);
        fv += w[k].nv, fi += w[k].ni, fs += w[k].ne;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].pos, (size_t)w[k].nv * 12);
    fv = 0;
    for (size_t k = 0; k < ne; k++) {
        for (uint32_t i = 0; i < w[k].ni; i++) {
            uint32_t ix = w[k].idx[i];
            if (ix >= w[k].nv) {
                err->offset = CC_NO_OFFSET;
                memcpy(err->msg, "mesh index out of range", 24);
                goto done;
            }
            put_u32(out, fv + ix);
        }
        fv += w[k].nv;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].edges, (size_t)w[k].ne * 24);
    static const int8_t zero[8];
    for (size_t k = 0; k < ne; k++)
        for (uint32_t i = 0; i < w[k].ne; i++) buf_append(out, i < w[k].nn ? w[k].normals[i] : zero, 8);
    if (local)
        for (size_t k = 0; k < ne; k++) {
            const cc_scene_element *e = &s.elements[k];
            static const float z12[12];
            put_u32(out, e->shape >= 0);
            if (e->shape >= 0) {
                buf_append(out, e->to_scene.x, 12), buf_append(out, e->to_scene.y, 12);
                buf_append(out, e->to_scene.z, 12), buf_append(out, e->to_scene.t, 12);
            } else buf_append(out, z12, 48);
        }
    if (out->oom) goto oom;
    rc = 0;
    goto done;
oom:
    err->offset = CC_NO_OFFSET;
    memcpy(err->msg, "out of memory", 14);
done:
    cc_scene_free(&s);
    arena_free(&tmp);
    return rc;
}

EXPORT("mesh") int wa_mesh(int h, int flags, cc_wasm_res *r) {
    Doc *d = doc(h, r);
    if (!d) return 404;
    r->value = d->rev;
    Buf b;
    buf_init(&b);
    cc_error err;
    if (bmsh(&d->m, d->rev, flags & 1, &b, &err)) {
        buf_free(&b);
        return text(r, 500, err.msg);
    }
    return answer(r, &b, 200);
}

/* ---- ids, numbers ---- */

EXPORT("guid_seed") void wa_guid_seed(uint64_t k) { cc_guid_seed(k); }
EXPORT("guid_entropy") void wa_guid_entropy(const void *p, size_t n) { cc_guid_entropy(p, n); }

EXPORT("fmt_f64") int wa_fmt_f64(double x, int debug, cc_wasm_res *r) {
    Buf b;
    buf_init(&b);
    r->value = 0;
    int rc = debug ? fmt_f64_debug(&b, x) : fmt_f64_json(&b, x);
    if (rc) {
        buf_free(&b);
        return text(r, 400, "not a finite number");
    }
    return answer(r, &b, 200);
}

EXPORT("parse_f64") double wa_parse_f64(const char *p, size_t n) { return cc_strtod(p, n); }

/* ---- the libc of the host call layer (wasm_api.h) ---- */

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

/* Stable merge sort (glibc's qsort is a merge sort too when it can allocate; the host's
 * comparators never tie anyway). Falls back to insertion sort without memory. */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *a = base;
    if (n < 2 || !size) return;
    unsigned char *t = malloc(n * size);
    if (!t) {
        unsigned char tmp[64];
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--)
                for (size_t k = 0; k < size; k += sizeof tmp) {
                    size_t m = size - k < sizeof tmp ? size - k : sizeof tmp;
                    memcpy(tmp, a + (j - 1) * size + k, m);
                    memcpy(a + (j - 1) * size + k, a + j * size + k, m);
                    memcpy(a + j * size + k, tmp, m);
                }
        return;
    }
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(a + j * size, a + i * size) < 0) memcpy(t + k++ * size, a + j++ * size, size);
                else memcpy(t + k++ * size, a + i++ * size, size);
            }
            while (i < mid) memcpy(t + k++ * size, a + i++ * size, size);
            while (j < hi) memcpy(t + k++ * size, a + j++ * size, size);
        }
        memcpy(a, t, n * size);
    }
    free(t);
}

/* %.<p>f of x as glibc prints it for the host's values (sizes, |x| < 2^63): the decimal digits
 * of x * 10^p rounded half to even. */
static void put_fixed(Buf *b, double x, int prec) {
    if (x != x) return buf_puts(b, "nan");
    if (x < 0) buf_putc(b, '-'), x = -x;
    if (x > 9.2e18) return buf_puts(b, "inf");
    double sc = 1;
    for (int i = 0; i < prec; i++) sc *= 10;
    double v = x * sc, f = __builtin_floor(v), d = v - f;
    uint64_t q = (uint64_t)f;
    if (d > 0.5 || (d == 0.5 && (q & 1))) q++;
    uint64_t ip = q, fp = 0, m = 1;
    for (int i = 0; i < prec; i++) m *= 10;
    ip = q / m, fp = q % m;
    buf_u64(b, ip);
    if (prec > 0) {
        buf_putc(b, '.');
        char dig[24];
        for (int i = prec - 1; i >= 0; i--) dig[i] = (char)('0' + fp % 10), fp /= 10;
        buf_append(b, dig, (size_t)prec);
    }
}

static void vformat(Buf *b, const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') {
            buf_putc(b, *f);
            continue;
        }
        f++;
        int prec = -1;
        if (*f == '.') {
            f++;
            if (*f == '*') prec = va_arg(ap, int), f++;
            else for (prec = 0; *f >= '0' && *f <= '9'; f++) prec = prec * 10 + (*f - '0');
        }
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            size_t n = strlen(s);
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            buf_append(b, s, n);
        } else if (*f == 'd') {
            int v = va_arg(ap, int);
            if (v < 0) buf_putc(b, '-'), buf_u64(b, (uint64_t)0 - (uint64_t)(int64_t)v);
            else buf_u64(b, (uint64_t)v);
        } else if (*f == 'u') buf_u64(b, va_arg(ap, unsigned));
        else if (f[0] == 'z' && f[1] == 'u') buf_u64(b, va_arg(ap, size_t)), f++;
        else if (f[0] == 'l' && f[1] == 'l' && f[2] == 'u') buf_u64(b, va_arg(ap, unsigned long long)), f += 2;
        else if (*f == 'f') put_fixed(b, va_arg(ap, double), prec < 0 ? 6 : prec);
        else if (*f == '%') buf_putc(b, '%');
        else return; /* not used by the host: stop */
    }
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    Buf b;
    buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    vformat(&b, fmt, ap);
    va_end(ap);
    if (b.oom) {
        buf_free(&b);
        if (cap) out[0] = 0;
        return -1;
    }
    if (cap) {
        size_t k = b.len < cap - 1 ? b.len : cap - 1;
        if (k) memcpy(out, b.p, k);
        out[k] = 0;
    }
    int n = (int)b.len;
    buf_free(&b);
    return n;
}

static Buf host_log_buf;
int fprintf(cc_wasm_file *f, const char *fmt, ...) {
    (void)f;
    size_t before = host_log_buf.len;
    va_list ap;
    va_start(ap, fmt);
    if (host_log_buf.len < (1u << 20)) vformat(&host_log_buf, fmt, ap); /* bounded */
    va_end(ap);
    return (int)(host_log_buf.len - before);
}

/* ---- the host call layer (host/calls.c, session.c, mesh.c) ---- */

Buf cc_wasm_saved;
int cc_wasm_has_saved;

/* Copies (p, n) into a NUL-terminated malloc'd string; NULL when out of memory. */
static char *zdup(const char *p, size_t n) {
    char *z = malloc(n + 1);
    if (!z) return NULL;
    if (n) memcpy(z, p, n);
    z[n] = 0;
    return z;
}

EXPORT("host_open_text") int wa_host_open_text(const char *name, size_t nn, const char *p, size_t n, cc_wasm_res *r) {
    r->value = 0;
    char *z = zdup(name, nn);
    if (!z) return text(r, 500, "out of memory");
    cc_error err;
    int rc = cc_session_open_text(nn ? z : "model.bimble", p, n, &err);
    free(z);
    if (rc) return text(r, 400, err.msg);
    r->value = cc_host_rev();
    return text(r, 200, "");
}

EXPORT("call") int wa_call(const char *op, size_t on, const char *name, size_t nn, const char *body, size_t bn,
                           cc_wasm_res *r) {
    r->value = 0;
    char *zop = zdup(op, on), *zname = zdup(name, nn), *zbody = zdup(body, bn);
    int st;
    Buf b;
    buf_init(&b);
    if (!zop || !zname || !zbody) st = 500, buf_puts(&b, "out of memory");
    else if (!cc_known_op(zop)) st = 404, buf_puts(&b, "unknown call"); /* server.c: not a route */
    else st = cc_call(zop, zname, zbody, bn, &b);
    free(zop), free(zname), free(zbody);
    return answer(r, &b, st);
}

EXPORT("change_json") int wa_change_json(cc_wasm_res *r) {
    r->value = cc_host_rev();
    Buf b;
    buf_init(&b);
    cc_change_json(&b);
    return answer(r, &b, 200);
}

EXPORT("host_mesh") int wa_host_mesh(int flags, cc_wasm_res *r) {
    if (flags & 1) { /* the local form: the walk above, on the session's model */
        const cc_model *m = cc_host_model();
        r->value = cc_host_rev();
        if (!m) return text(r, 404, "no model open");
        Buf b;
        buf_init(&b);
        cc_error err;
        if (bmsh(m, cc_host_rev(), 1, &b, &err)) {
            buf_free(&b);
            return text(r, 500, err.msg);
        }
        return answer(r, &b, 200);
    }
    const char *p;
    size_t n;
    uint32_t rev;
    double ms;
    int st = cc_mesh_get(&p, &n, &rev, &ms);
    r->value = rev;
    Buf b;
    buf_init(&b);
    buf_append(&b, p, n);
    return answer(r, &b, st);
}

EXPORT("host_saved") int wa_host_saved(cc_wasm_res *r) {
    r->value = 0;
    if (!cc_wasm_has_saved) return text(r, 404, "not saved yet");
    Buf b;
    buf_init(&b);
    buf_append(&b, cc_wasm_saved.p, cc_wasm_saved.len);
    return answer(r, &b, 200);
}

EXPORT("host_log") int wa_host_log(cc_wasm_res *r) {
    r->value = 0;
    Buf b = host_log_buf;
    buf_init(&host_log_buf);
    return answer(r, &b, 200);
}
#endif
