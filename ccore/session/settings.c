/* The user's settings (ticket SET1): settings.h. */
#include "settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "fmt.h"
#include "georef.h"
#include "session.h"
#include "settings_file.h"
#include "view_only.h"

void cc_settings_default(cc_settings *s) { /* settings.rs:43-58 */
    memset(s, 0, sizeof *s);
    s->memory_share = 0.8;
    s->moving_fps = 30;
    s->moving_proxies = 1;
    s->gpu_cull = 0;
    s->draw_visible = 0; /* VIS1: off until it is exact on a real project of links (ADR 0078 amendment 7) */
    strcpy(s->edge_mode, "screen"), s->cull_back = 0, s->still_px = 0, s->refill_offscreen = 1; /* VIS1 */
    strcpy(s->theme, "grey");    /* ui_theme::THEMES[0] */
    strcpy(s->ui_font, "roboto"); /* UiFont::Roboto */
    s->text_size = 12.0f;
    strcpy(s->coordinates, "survey");
    strcpy(s->renderer, "auto"); /* GPU1 */
    s->link_gpu_mb = CC_LINK_GPU_MB_DEFAULT; /* SIZE3 */
    strcpy(s->link_detail, "standard"); /* TESS1 */
    s->link_chord_mm = 2.0, s->link_max_deg = 18.0;
}

#ifndef CC_VIEW_ONLY /* the viewer-only wasm has no settings (the 400 KB budget): the stubs at the end */

/* ui_theme.rs THEMES (the default first) and UiFont::ALL. */
static const char *const THEMES[] = {"grey", "dark", "light", "solarized-dark", "solarized-light", "high-contrast"};
static const char *const FONTS[] = {"roboto", "ubuntu"};
static const char *const RENDERERS[] = {"auto", "webgl2", "webgpu"}; /* GPU1: the lab's 3D renderer */
static const char *const EDGE_MODES[] = {"screen", "lines", "none"}; /* VIS1: the 3D view's edges */
#define NEDGE_MODES (sizeof EDGE_MODES / sizeof EDGE_MODES[0])
#define STILL_PX_MAX 8.0
#define NTHEMES (sizeof THEMES / sizeof THEMES[0])
#define NFONTS (sizeof FONTS / sizeof FONTS[0])
#define NRENDERERS (sizeof RENDERERS / sizeof RENDERERS[0])
static const char *const LINK_DETAILS[] = {"exact", "standard", "light", "custom"}; /* TESS1: cc_ifc_detail's order */
#define NLINK_DETAILS (sizeof LINK_DETAILS / sizeof LINK_DETAILS[0])
#define TEXT_MIN 10.0f /* ui_theme.rs TEXT_SIZES */
#define TEXT_MAX 16.0f

/* The fields, sorted as serde_json writes them (a BTreeMap), `recent` included. */
static const char *const FIELDS[] = {"coordinates", "gpu_cull", "memory_share", "moving_fps", "moving_proxies", "recent",
                                     "text_size", "theme", "ui_font", "user_email", "user_name"};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])


/* A formatted line appended (snprintf: the freestanding one knows %s %d %u %zu). */
static void say(Buf *b, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

/* ---- UTF-8 and the few Unicode rules Rust's str methods use ---- */

/* The character at s[*i] (valid UTF-8: jr_parse checked it), *i moved past it. */
static unsigned next_cp(const char *s, size_t n, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[*i];
    size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (*i + len > n) len = n - *i;
    unsigned cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (size_t k = 1; k < len; k++) cp = cp << 6 | (u[*i + k] & 0x3f);
    *i += len;
    return cp;
}

static void put_cp(Buf *b, unsigned cp) {
    char t[4];
    size_t n;
    if (cp < 0x80) t[0] = (char)cp, n = 1;
    else if (cp < 0x800) t[0] = (char)(0xc0 | cp >> 6), t[1] = (char)(0x80 | (cp & 0x3f)), n = 2;
    else if (cp < 0x10000) t[0] = (char)(0xe0 | cp >> 12), t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), t[2] = (char)(0x80 | (cp & 0x3f)), n = 3;
    else
        t[0] = (char)(0xf0 | cp >> 18), t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)), t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)),
        t[3] = (char)(0x80 | (cp & 0x3f)), n = 4;
    buf_append(b, t, n);
}

/* char::is_control: the Cc category. */
static int is_control(unsigned c) { return c < 0x20 || (c >= 0x7f && c <= 0x9f); }

/* char::is_whitespace: Unicode White_Space. */
static int is_ws(unsigned c) {
    return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

/* char::to_uppercase for Latin, Greek and Cyrillic letters (DEVIATIONS 23: other scripts are
 * kept as they are). 0xdf (ß) is "SS": the caller writes two. */
static unsigned upper(unsigned c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c == 0xb5) return 0x39c;
    if ((c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32;
    if (c == 0xff) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) return c & ~1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c : c - 1;
    if (c == 0x17f) return 'S';
    if (c == 0x3c2) return 0x3a3;
    if (c >= 0x3b1 && c <= 0x3cb) return c - 32;
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c >= 0x450 && c <= 0x45f) return c - 80;
    return c;
}

int cc_settings_trigram(const char *name, char *out, size_t cap) { /* ribbon.rs:437 */
    size_t n = strlen(name), i = 0;
    unsigned first[3], one[3];
    size_t nwords = 0, none = 0;
    int in_word = 0;
    while (i < n) {
        unsigned c = next_cp(name, n, &i);
        if (is_ws(c)) {
            in_word = 0;
            continue;
        }
        if (!in_word) {
            if (nwords < 3) first[nwords] = c;
            nwords++;
            in_word = 1;
        }
        if (nwords == 1 && none < 3) one[none++] = c; /* the first word's first three characters */
    }
    if (!nwords) return 0;
    const unsigned *t = nwords == 1 ? one : first;
    size_t k = nwords == 1 ? none : (nwords < 3 ? nwords : 3);
    Buf b;
    buf_init(&b);
    for (size_t j = 0; j < k; j++) {
        if (t[j] == 0xdf) buf_puts(&b, "SS");
        else put_cp(&b, upper(t[j]));
    }
    int ok = !b.oom && b.len < cap;
    if (ok) memcpy(out, b.p, b.len), out[b.len] = 0;
    buf_free(&b);
    return ok;
}

/* settings.rs short_line: the characters that are not control characters, the first 120. */
static void short_line(const JrVal *v, char *out) {
    out[0] = 0;
    if (!v || v->type != JR_STR) return;
    size_t i = 0, o = 0, nchars = 0;
    while (i < v->n && nchars < CC_SETTINGS_LINE) {
        size_t at = i;
        unsigned c = next_cp(v->s, v->n, &i);
        if (is_control(c)) continue;
        memcpy(out + o, v->s + at, i - at), o += i - at, nchars++;
    }
    out[o] = 0;
}

/* serde_json's `v[key]`: the last member of that key (a map keeps the last), NULL when absent. */
static const JrVal *field(const JrVal *v, const char *key) {
    const JrVal *r = NULL;
    for (size_t i = 0; v && v->type == JR_OBJ && i < v->n; i++)
        if (jr_key_is(&v->members[i], key)) r = &v->members[i].val;
    return r;
}

static int str_is(const JrVal *v, const char *s) { return v && v->type == JR_STR && v->n == strlen(s) && !memcmp(v->s, s, v->n); }

static int known(const char *const *ids, size_t n, const JrVal *v) {
    for (size_t i = 0; v && v->type == JR_STR && i < n; i++)
        if (str_is(v, ids[i])) return (int)i;
    return -1;
}

/* `u64::as_u64` of a JSON number: a non-negative integer token that fits; 0 or -1. */
static int as_u64(const JrVal *v, unsigned long long *out) {
    if (!v || v->type != JR_NUM || !v->num_int || v->num_neg) return -1;
    if (v->n > 20 || (v->n == 20 && memcmp(v->s, "18446744073709551615", 20) > 0)) return -1;
    unsigned long long x = 0;
    for (size_t i = 0; i < v->n; i++) x = x * 10 + (unsigned)(v->s[i] - '0');
    *out = x;
    return 0;
}

static int finite(double x) { return x - x == 0.0; }

/* "epsg:<n>" with n a u16 (Rust's `parse::<u16>`: an optional '+', digits) in Rust's table. */
static int epsg_ok(const char *s, size_t n) {
    if (n < 6 || memcmp(s, "epsg:", 5)) return 0;
    size_t i = 5;
    if (s[i] == '+') i++;
    if (i == n) return 0;
    unsigned long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (unsigned long)(s[i] - '0');
        if (v > 65535) return 0;
    }
    return cc_crs_known((int)v);
}

void cc_settings_of(const JrVal *v, cc_settings *s) { /* settings.rs:70-125 from_json */
    cc_settings_default(s);
    if (!v || v->type != JR_OBJ) return;
    const JrVal *f = field(v, "memory_share");
    if (f && f->type == JR_NUM && finite(f->num)) s->memory_share = f->num < 0.2 ? 0.2 : f->num > 1.0 ? 1.0 : f->num;
    unsigned long long u;
    if (!as_u64(field(v, "moving_fps"), &u)) s->moving_fps = u > 240 ? 240 : (unsigned)u;
    if ((f = field(v, "moving_proxies")) && f->type == JR_BOOL) s->moving_proxies = f->b;
    if ((f = field(v, "gpu_cull")) && f->type == JR_BOOL) s->gpu_cull = f->b;
    if ((f = field(v, "draw_visible")) && f->type == JR_BOOL) s->draw_visible = f->b; /* VIS1 */
    if ((f = field(v, "cull_back")) && f->type == JR_BOOL) s->cull_back = f->b;
    if ((f = field(v, "refill_offscreen")) && f->type == JR_BOOL) s->refill_offscreen = f->b;
    if ((f = field(v, "still_px")) && f->type == JR_NUM && finite(f->num)) s->still_px = f->num < 0 ? 0 : f->num > STILL_PX_MAX ? STILL_PX_MAX : f->num;
    { int e = known(EDGE_MODES, NEDGE_MODES, field(v, "edge_mode")); if (e >= 0) strcpy(s->edge_mode, EDGE_MODES[e]); }
    if ((f = field(v, "theme")) && f->type == JR_STR) { /* by_id: an unknown id is the default */
        int k = known(THEMES, NTHEMES, f);
        strcpy(s->theme, THEMES[k < 0 ? 0 : k]);
    }
    int k = known(FONTS, NFONTS, field(v, "ui_font"));
    if (k >= 0) strcpy(s->ui_font, FONTS[k]);
    if ((f = field(v, "text_size")) && f->type == JR_NUM && finite(f->num)) {
        float x = (float)f->num;
        s->text_size = x < TEXT_MIN ? TEXT_MIN : x > TEXT_MAX ? TEXT_MAX : x;
    }
    short_line(field(v, "user_name"), s->user_name);
    short_line(field(v, "user_email"), s->user_email);
    f = field(v, "coordinates");
    if (str_is(f, "survey") || str_is(f, "wgs84") || str_is(f, "utm") || (f && f->type == JR_STR && epsg_ok(f->s, f->n)))
        memcpy(s->coordinates, f->s, f->n), s->coordinates[f->n] = 0;
    else if ((f = field(v, "wgs84")) && f->type == JR_BOOL && f->b) strcpy(s->coordinates, "wgs84"); /* 0.14's setting */
    if ((k = known(RENDERERS, NRENDERERS, field(v, "renderer"))) >= 0) strcpy(s->renderer, RENDERERS[k]); /* GPU1 */
    if (!as_u64(field(v, "link_gpu_mb"), &u)) s->link_gpu_mb = u < 64 ? 64 : u > 65536 ? 65536 : (unsigned)u; /* SIZE3 */
    if ((k = known(LINK_DETAILS, NLINK_DETAILS, field(v, "link_detail"))) >= 0) strcpy(s->link_detail, LINK_DETAILS[k]); /* TESS1 */
    if ((f = field(v, "link_chord_mm")) && f->type == JR_NUM && f->num >= CC_IFC_CHORD_MM_MIN && f->num <= CC_IFC_CHORD_MM_MAX)
        s->link_chord_mm = f->num;
    if ((f = field(v, "link_max_deg")) && f->type == JR_NUM && f->num >= CC_IFC_MAX_DEG_MIN && f->num <= CC_IFC_MAX_DEG_MAX)
        s->link_max_deg = f->num;
    if ((f = field(v, "open_app")) && f->type == JR_BOOL) s->open_app = f->b; /* GPU2 */
}

int cc_settings_parse(const char *text, size_t n, cc_settings *s) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = text && n ? jr_parse(&a, text, n, &je) : NULL;
    cc_settings_of(v, s);
    arena_free(&a);
    return v ? 0 : -1;
}

/* ---- set_settings: checked as a whole, then applied ---- */

static void line_check(const JrVal *v, const char *name, char *dst, Buf *why) {
    if (v->type != JR_STR) {
        say(why, "`%s`: a string", name);
        return;
    }
    size_t i = 0, nchars = 0;
    while (i < v->n) {
        unsigned c = next_cp(v->s, v->n, &i);
        if (is_control(c)) {
            say(why, "`%s`: one line, no control characters", name);
            return;
        }
        nchars++;
    }
    if (nchars > CC_SETTINGS_LINE) {
        say(why, "`%s`: at most %d characters (it is written into every commit), not %zu", name, CC_SETTINGS_LINE, nchars);
        return;
    }
    memcpy(dst, v->s, v->n), dst[v->n] = 0;
}

int cc_settings_set(cc_settings *s, const JrVal *body, Buf *why) {
    if (!body || body->type != JR_OBJ) {
        buf_puts(why, "set_settings: {field: value, ...}");
        return -1;
    }
    cc_settings t = *s;
    size_t start = why->len;
    for (size_t i = 0; i < body->n && why->len == start; i++) {
        const JrMember *m = &body->members[i];
        const JrVal *v = &m->val;
        if (jr_key_is(m, "memory_share")) {
            if (v->type != JR_NUM || !finite(v->num) || v->num < 0.2 || v->num > 1.0) buf_puts(why, "`memory_share`: a number from 0.2 to 1.0");
            else t.memory_share = v->num;
        } else if (jr_key_is(m, "moving_fps")) {
            unsigned long long u;
            if (as_u64(v, &u) || u > 240) buf_puts(why, "`moving_fps`: a whole number from 0 to 240 (0: draw everything)");
            else t.moving_fps = (unsigned)u;
        } else if (jr_key_is(m, "moving_proxies") || jr_key_is(m, "gpu_cull") || jr_key_is(m, "draw_visible") || jr_key_is(m, "cull_back") || jr_key_is(m, "refill_offscreen")) {
            if (v->type != JR_BOOL) say(why, "`%s`: true or false", m->key);
            else *(jr_key_is(m, "gpu_cull") ? &t.gpu_cull : jr_key_is(m, "draw_visible") ? &t.draw_visible : jr_key_is(m, "cull_back") ? &t.cull_back
                   : jr_key_is(m, "refill_offscreen") ? &t.refill_offscreen : &t.moving_proxies) = v->b;
        } else if (jr_key_is(m, "edge_mode")) { /* VIS1 */
            int k = known(EDGE_MODES, NEDGE_MODES, v);
            if (k < 0) buf_puts(why, "`edge_mode`: screen, lines or none");
            else strcpy(t.edge_mode, EDGE_MODES[k]);
        } else if (jr_key_is(m, "still_px")) { /* VIS1 */
            if (v->type != JR_NUM || !finite(v->num) || v->num < 0 || v->num > STILL_PX_MAX) buf_puts(why, "`still_px`: pixels, from 0 (off) to 8");
            else t.still_px = v->num;
        } else if (jr_key_is(m, "theme")) {
            int k = known(THEMES, NTHEMES, v);
            if (k < 0) buf_puts(why, "`theme`: one of grey, dark, light, solarized-dark, solarized-light, high-contrast");
            else strcpy(t.theme, THEMES[k]);
        } else if (jr_key_is(m, "ui_font")) {
            int k = known(FONTS, NFONTS, v);
            if (k < 0) buf_puts(why, "`ui_font`: roboto or ubuntu");
            else strcpy(t.ui_font, FONTS[k]);
        } else if (jr_key_is(m, "text_size")) {
            if (v->type != JR_NUM || !finite(v->num) || v->num < TEXT_MIN || v->num > TEXT_MAX) buf_puts(why, "`text_size`: points, from 10 to 16");
            else t.text_size = (float)v->num;
        } else if (jr_key_is(m, "user_name")) line_check(v, "user_name", t.user_name, why);
        else if (jr_key_is(m, "user_email")) line_check(v, "user_email", t.user_email, why);
        else if (jr_key_is(m, "coordinates")) {
            if (str_is(v, "survey") || str_is(v, "wgs84") || str_is(v, "utm") || (v->type == JR_STR && epsg_ok(v->s, v->n)))
                memcpy(t.coordinates, v->s, v->n), t.coordinates[v->n] = 0;
            else buf_puts(why, "`coordinates`: \"survey\", \"wgs84\", \"utm\" or \"epsg:<n>\" of a system Bimble knows");
        } else if (jr_key_is(m, "renderer")) { /* GPU1 */
            int k = known(RENDERERS, NRENDERERS, v);
            if (k < 0) buf_puts(why, "`renderer`: auto, webgl2 or webgpu");
            else strcpy(t.renderer, RENDERERS[k]);
        } else if (jr_key_is(m, "link_detail")) { /* TESS1 */
            int k = known(LINK_DETAILS, NLINK_DETAILS, v);
            if (k < 0) buf_puts(why, "`link_detail`: exact, standard, light or custom");
            else strcpy(t.link_detail, LINK_DETAILS[k]);
        } else if (jr_key_is(m, "link_chord_mm")) { /* TESS1 */
            if (v->type != JR_NUM || !(v->num >= CC_IFC_CHORD_MM_MIN && v->num <= CC_IFC_CHORD_MM_MAX))
                buf_puts(why, "`link_chord_mm`: millimetres from 0.01 to 1000");
            else t.link_chord_mm = v->num;
        } else if (jr_key_is(m, "link_max_deg")) { /* TESS1 */
            if (v->type != JR_NUM || !(v->num >= CC_IFC_MAX_DEG_MIN && v->num <= CC_IFC_MAX_DEG_MAX))
                buf_puts(why, "`link_max_deg`: degrees from 1 to 90");
            else t.link_max_deg = v->num;
        } else if (jr_key_is(m, "link_gpu_mb")) { /* SIZE3 */
            unsigned long long u;
            if (as_u64(v, &u) || u < 64 || u > 65536) buf_puts(why, "`link_gpu_mb`: a whole number of MB from 64 to 65536");
            else t.link_gpu_mb = (unsigned)u;
        } else if (jr_key_is(m, "open_app")) { /* GPU2 */
            if (v->type != JR_BOOL) buf_puts(why, "`open_app`: true or false");
            else t.open_app = v->b;
        } else if (jr_key_is(m, "recent")) buf_puts(why, "`recent` changes by opening a file, or with the action recent_remove");
        else {
            buf_puts(why, "unknown field `");
            buf_append(why, m->key, m->key_n < 64 ? m->key_n : 64);
            buf_puts(why, "`, expected one of `coordinates`, `cull_back`, `draw_visible`, `edge_mode`, `gpu_cull`, `link_chord_mm`, `link_detail`, `link_gpu_mb`, `link_max_deg`, `memory_share`, `moving_fps`, `moving_proxies`, `open_app`, `refill_offscreen`, `renderer`, `still_px`, `text_size`, "
                          "`theme`, `ui_font`, `user_email`, `user_name`");
        }
    }
    if (why->len != start || why->oom) return -1;
    *s = t;
    return 0;
}

/* ---- to_json ---- */

/* The value of field FIELDS[k] (not `recent`). */
static void field_value(JsonW *w, const cc_settings *s, size_t k) {
    switch (k) {
    case 0: jw_str(w, s->coordinates); break;
    case 1: jw_bool(w, s->gpu_cull); break;
    case 2: jw_f64(w, s->memory_share); break;
    case 3: jw_u64(w, s->moving_fps); break;
    case 4: jw_bool(w, s->moving_proxies); break;
    case 6: jw_f64(w, (double)s->text_size); break; /* f32 to a JSON f64, as serde_json's Value::from(f32) */
    case 7: jw_str(w, s->theme); break;
    case 8: jw_str(w, s->ui_font); break;
    case 9: jw_str(w, s->user_email); break;
    case 10: jw_str(w, s->user_name); break;
    }
}

static void values(JsonW *w, const cc_settings *s, char *const *recent, size_t nrecent, int with_renderer) {
    jw_obj_begin(w);
    for (size_t k = 0; k < NFIELDS; k++) {
        if (k == 2 && with_renderer) jw_key(w, "link_chord_mm"), jw_f64(w, s->link_chord_mm); /* TESS1 */
        if (k == 2 && with_renderer) jw_key(w, "link_detail"), jw_str(w, s->link_detail); /* TESS1: before link_gpu_mb */
        if (k == 1 && with_renderer) { /* VIS1: sorted between coordinates and gpu_cull */
            jw_key(w, "cull_back"), jw_bool(w, s->cull_back);
            jw_key(w, "draw_visible"), jw_bool(w, s->draw_visible);
            jw_key(w, "edge_mode"), jw_str(w, s->edge_mode);
        }
        if (k == 6 && with_renderer) jw_key(w, "refill_offscreen"), jw_bool(w, s->refill_offscreen); /* VIS1: before renderer */
        if (k == 2 && with_renderer) jw_key(w, "link_gpu_mb"), jw_u64(w, s->link_gpu_mb); /* SIZE3: sorted between gpu_cull and memory_share */
        if (k == 2 && with_renderer) jw_key(w, "link_max_deg"), jw_f64(w, s->link_max_deg); /* TESS1 */
        if (k == 5 && with_renderer) jw_key(w, "open_app"), jw_bool(w, s->open_app); /* GPU2: sorted between moving_proxies and recent */
        if (k == 6 && with_renderer) jw_key(w, "renderer"), jw_str(w, s->renderer); /* GPU1: sorted between recent and text_size */
        if (k == 6 && with_renderer) jw_key(w, "still_px"), jw_f64(w, s->still_px); /* VIS1: after renderer */
        jw_key(w, FIELDS[k]);
        if (k != 5) {
            field_value(w, s, k);
            continue;
        }
        jw_arr_begin(w);
        for (size_t i = 0; i < nrecent; i++) jw_str(w, recent[i]);
        jw_arr_end(w);
    }
    jw_obj_end(w);
}

void cc_settings_to_json(const cc_settings *s, char *const *recent, size_t nrecent, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    values(&w, s, recent, nrecent, 0);
    if (jw_finish(&w) != JW_OK) out->oom = 1;
}

/* ---- the session's settings ---- */

struct cc_user_settings {
    char *mem;   /* the text in memory: no file, or a change the file would not take */
    size_t mem_n;
    int in_mem;  /* read from `mem`, not the file */
    char note[256];
};

static struct cc_user_settings *us(cc_session *s) {
    if (!s->user_settings) s->user_settings = calloc(1, sizeof *s->user_settings);
    return s->user_settings;
}

void cc_settings_free(cc_session *s) {
    if (!s->user_settings) return;
    free(s->user_settings->mem);
    free(s->user_settings);
    s->user_settings = NULL;
}

/* The settings now: the file's (read again: the Rust app may have changed it) or memory's.
 * `text`: the text they come from (malloc'd, NULL: none), for a write that keeps the rest. */
typedef struct {
    cc_settings v;
    char *recent[CC_RECENT_MAX];
    size_t nrecent;
    int has_recent; /* the text has a "recent" member */
    int has_renderer; /* GPU1: the text has a "renderer" member */
    int has_draw_visible; /* VIS1: the text has a "draw_visible" member */
    int has_vis1[4]; /* VIS1: edge_mode, cull_back, still_px, refill_offscreen in the text */
    int has_link_gpu; /* SIZE3: the text has a "link_gpu_mb" member */
    int has_link_detail; /* TESS1: the text has a "link_detail" member */
    int has_link_chord, has_link_deg; /* TESS1: "link_chord_mm", "link_max_deg" */
    int has_open_app; /* GPU2: the text has an "open_app" member */
    char *text;
    size_t n;
} Now;

static void now_free(Now *c) {
    for (size_t i = 0; i < c->nrecent; i++) free(c->recent[i]);
    free(c->text);
}

static int now_read(cc_session *s, Now *c) {
    memset(c, 0, sizeof *c);
    struct cc_user_settings *u = us(s);
    if (!u) return -1;
    if (s->settings_file && !u->in_mem) {
        int st = s->pf->read_file(s->pf->ctx, s->settings_file, CC_SETTINGS_MAX_BYTES, &c->text, &c->n);
        if (st == CC_PF_NOT_FOUND) u->note[0] = 0;
        else if (st != CC_PF_OK) snprintf(u->note, sizeof u->note, "%s does not read: the defaults", s->settings_file);
    } else if (u->mem) {
        c->text = malloc(u->mem_n + 1);
        if (!c->text) return -1;
        memcpy(c->text, u->mem, u->mem_n), c->text[u->mem_n] = 0, c->n = u->mem_n;
    }
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = c->text && c->n ? jr_parse(&a, c->text, c->n, &je) : NULL;
    cc_settings_of(v, &c->v);
    if (v) {
        cc_settings_recent_of(v, c->recent, &c->nrecent);
        c->has_recent = v->type == JR_OBJ && field(v, "recent") != NULL;
        c->has_renderer = v->type == JR_OBJ && field(v, "renderer") != NULL;
        c->has_draw_visible = v->type == JR_OBJ && field(v, "draw_visible") != NULL;
        { static const char *const K[4] = {"edge_mode", "cull_back", "still_px", "refill_offscreen"};
          for (int i = 0; i < 4; i++) c->has_vis1[i] = v->type == JR_OBJ && field(v, K[i]) != NULL; }
        c->has_link_gpu = v->type == JR_OBJ && field(v, "link_gpu_mb") != NULL; /* SIZE3 */
        c->has_link_detail = v->type == JR_OBJ && field(v, "link_detail") != NULL; /* TESS1 */
        c->has_link_chord = v->type == JR_OBJ && field(v, "link_chord_mm") != NULL;
        c->has_link_deg = v->type == JR_OBJ && field(v, "link_max_deg") != NULL;
        c->has_open_app = v->type == JR_OBJ && field(v, "open_app") != NULL;
        if (!u->in_mem && v->type != JR_OBJ) snprintf(u->note, sizeof u->note, "the settings are not a JSON object: the defaults");
        else if (!u->in_mem) u->note[0] = 0;
    } else if (c->text && c->n && !u->in_mem) /* settings.rs: broken JSON reads as the defaults */
        snprintf(u->note, sizeof u->note, "the settings are not JSON (%s): the defaults", je.msg);
    arena_free(&a);
    return 0;
}

void cc_settings_user_json(cc_session *s, JsonW *w) { /* calls.rs:253 */
    Now c;
    char t[16];
    if (now_read(s, &c) || !cc_settings_trigram(c.v.user_name, t, sizeof t)) {
        now_free(&c);
        jw_null(w);
        return;
    }
    /* trim(): Unicode white space at both ends */
    const char *fields[2] = {c.v.user_email, c.v.user_name};
    size_t from[2], to[2];
    for (int k = 0; k < 2; k++) {
        const char *p = fields[k];
        size_t n = strlen(p), i = 0, first = n, last = 0;
        while (i < n) {
            size_t at = i;
            if (!is_ws(next_cp(p, n, &i))) {
                if (first == n) first = at;
                last = i;
            }
        }
        from[k] = first == n ? 0 : first, to[k] = first == n ? 0 : last;
    }
    jw_obj_begin(w);
    jw_key(w, "email");
    if (to[0] > from[0]) jw_str_n(w, c.v.user_email + from[0], to[0] - from[0]);
    else jw_null(w);
    jw_key(w, "name"), jw_str_n(w, c.v.user_name + from[1], to[1] - from[1]);
    jw_key(w, "trigram"), jw_str(w, t);
    jw_obj_end(w);
    now_free(&c);
}

int cc_settings_query(cc_session *s, Buf *out) {
    Now c;
    if (now_read(s, &c)) return out->oom = 1, -1;
    struct cc_user_settings *u = s->user_settings;
    int files = s->pf->files;
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    /* What this build does with each field: the page applies the look, the readout and the user;
     * the viewport is the page's WebGL one, natively too (ADR 0066): its moving budget follows
     * moving_fps (ticket 3D1, ADR 0078), its far cells' stand-ins moving_proxies (3D2); no memory
     * budget; GPU culling with the WebGPU renderer only (GPU1, below). */
    static const signed char APPLIES[NFIELDS] = {1, 0, 0, 1, 1, -1, 1, 1, 1, 1, 1}; /* -1: with files; 3D2: moving_proxies */
    /* GPU1 (ADR 0083): gpu_cull applies with the WebGPU renderer (compute shaders); renderer always */
    int webgpu = !strcmp(c.v.renderer, "webgpu");
    jw_key(&w, "applies"), jw_obj_begin(&w);
    for (size_t k = 0; k < NFIELDS; k++) {
        if (k == 1) jw_key(&w, "cull_back"), jw_bool(&w, 1), jw_key(&w, "draw_visible"), jw_bool(&w, 1), jw_key(&w, "edge_mode"), jw_bool(&w, 1); /* VIS1: the WebGL2 3D view */
        if (k == 2) jw_key(&w, "link_chord_mm"), jw_bool(&w, 1); /* TESS1: the links' conversion */
        if (k == 2) jw_key(&w, "link_detail"), jw_bool(&w, 1);
        if (k == 2) jw_key(&w, "link_gpu_mb"), jw_bool(&w, 1); /* SIZE3: the WebGL2 viewport's budget across links */
        if (k == 2) jw_key(&w, "link_max_deg"), jw_bool(&w, 1); /* TESS1 */
        if (k == 5) jw_key(&w, "open_app"), jw_bool(&w, files); /* GPU2: the native host's `open` reads it */
        if (k == 6) jw_key(&w, "refill_offscreen"), jw_bool(&w, 1);
        if (k == 6) jw_key(&w, "renderer"), jw_bool(&w, 1), jw_key(&w, "still_px"), jw_bool(&w, 1);
        jw_key(&w, FIELDS[k]), jw_bool(&w, k == 1 ? webgpu : APPLIES[k] < 0 ? files : APPLIES[k]);
    }
    jw_obj_end(&w);
    jw_key(&w, "file");
    if (s->settings_file) jw_str(&w, s->settings_file);
    else jw_null(&w);
    jw_key(&w, "fonts"), jw_arr_begin(&w);
    for (size_t k = 0; k < NFONTS; k++) jw_str(&w, FONTS[k]);
    jw_arr_end(&w);
    jw_key(&w, "needs"), jw_obj_begin(&w);
    jw_key(&w, "gpu_cull"), jw_str(&w, "the WebGPU renderer (3D renderer: WebGPU, ADR 0083): the copies are tested by a compute shader; WebGL2 has none");
    jw_key(&w, "memory_share"),
        jw_str(&w, "a memory budget per model in the C core, and an open that asks which part to show past it (ADR 0033)");
    if (!files) jw_key(&w, "recent"), jw_str(&w, "a file system: the browser keeps no path to open again");
    jw_obj_end(&w);
    jw_key(&w, "note");
    if (u && u->note[0]) jw_str(&w, u->note);
    else jw_null(&w);
    jw_key(&w, "text_sizes"), jw_arr_begin(&w), jw_f64(&w, TEXT_MIN), jw_f64(&w, TEXT_MAX), jw_arr_end(&w);
    jw_key(&w, "themes"), jw_arr_begin(&w);
    for (size_t k = 0; k < NTHEMES; k++) jw_str(&w, THEMES[k]);
    jw_arr_end(&w);
    jw_key(&w, "values");
    values(&w, &c.v, c.recent, c.nrecent, 1);
    jw_obj_end(&w);
    now_free(&c);
    return jw_finish(&w) == JW_OK ? 0 : (out->oom = 1, -1);
}

void cc_settings_link_opts(cc_session *s, cc_ifc_read_opts *o) { /* TESS1 */
    o->detail = CC_IFC_DETAIL_STANDARD, o->chord_mm = 2.0, o->max_deg = 18.0;
    Now c;
    if (now_read(s, &c)) return;
    for (size_t i = 0; i < NLINK_DETAILS; i++)
        if (!strcmp(c.v.link_detail, LINK_DETAILS[i])) o->detail = (cc_ifc_detail)i;
    o->chord_mm = c.v.link_chord_mm, o->max_deg = c.v.link_max_deg;
    now_free(&c);
}

int cc_settings_text_query(cc_session *s, Buf *out) {
    Now c;
    if (now_read(s, &c)) return out->oom = 1, -1;
    if (c.text) json_escape(out, c.text, c.n); /* the text as a JSON string: the answer stays JSON */
    else buf_puts(out, "null");
    now_free(&c);
    return out->oom ? -1 : 0;
}

/* Rust's to_json over the text the settings came from: every field Rust writes, the text's other
 * members kept (a text with no "recent" gets the session's list, as Rust writes it). */
static int write_members(cc_session *s, const Now *c, const cc_settings *v) {
    Buf vals[NFIELDS];
    cc_settings_member m[NFIELDS + 14]; /* the lab fields: GPU1, SIZE3, TESS1, GPU2, VIS1 */
    char ej[16], sj[32]; /* VIS1: edge_mode's and still_px's JSON */
    snprintf(ej, sizeof ej, "\"%s\"", v->edge_mode);
    { Buf b; buf_init(&b); JsonW jw; jw_init(&jw, &b, 0); jw_f64(&jw, v->still_px); jw_finish(&jw); snprintf(sj, sizeof sj, "%.*s", (int)b.len, b.p ? b.p : "0"); buf_free(&b); }
    size_t nm = 0;
    int rc = 0;
    char rj[16]; /* GPU1: "renderer"'s JSON */
    snprintf(rj, sizeof rj, "\"%s\"", v->renderer);
    char gj[16]; /* SIZE3: "link_gpu_mb"'s JSON */
    char dj[16]; /* TESS1: "link_detail"'s JSON, "link_chord_mm" and "link_max_deg" */
    snprintf(dj, sizeof dj, "\"%s\"", v->link_detail);
    Buf cjb, mjb;
    buf_init(&cjb), buf_init(&mjb);
    {
        JsonW jw;
        jw_init(&jw, &cjb, 0), jw_f64(&jw, v->link_chord_mm), jw_finish(&jw);
        jw_init(&jw, &mjb, 0), jw_f64(&jw, v->link_max_deg), jw_finish(&jw);
    }
    snprintf(gj, sizeof gj, "%u", v->link_gpu_mb);
    for (size_t k = 0; k < NFIELDS; k++) {
        /* SIZE3: the links' GPU budget, once set or already in the text (sorted: after gpu_cull) */
        /* TESS1: the links' detail, once set or already in the text (sorted: before link_gpu_mb) */
        if (k == 2 && (v->link_chord_mm != 2.0 || c->has_link_chord)) m[nm++] = (cc_settings_member){"link_chord_mm", cjb.p, cjb.len};
        if (k == 2 && (strcmp(v->link_detail, "standard") || c->has_link_detail)) m[nm++] = (cc_settings_member){"link_detail", dj, strlen(dj)};
        if (k == 2 && (v->link_gpu_mb != CC_LINK_GPU_MB_DEFAULT || c->has_link_gpu)) m[nm++] = (cc_settings_member){"link_gpu_mb", gj, strlen(gj)};
        if (k == 2 && (v->link_max_deg != 18.0 || c->has_link_deg)) m[nm++] = (cc_settings_member){"link_max_deg", mjb.p, mjb.len};
        /* GPU2: open_app, once set or already in the text (sorted: before recent) */
        if (k == 5 && (v->open_app || c->has_open_app)) m[nm++] = (cc_settings_member){"open_app", v->open_app ? "true" : "false", v->open_app ? 4u : 5u};
        /* VIS1: the 3D view's optimisations, each once set from its default or already in the text (sorted) */
        if (k == 1 && (v->cull_back || c->has_vis1[1])) m[nm++] = (cc_settings_member){"cull_back", v->cull_back ? "true" : "false", v->cull_back ? 4u : 5u};
        /* VIS1: draw_visible, once set off or already in the text (sorted: after coordinates) */
        if (k == 1 && (v->draw_visible || c->has_draw_visible)) m[nm++] = (cc_settings_member){"draw_visible", v->draw_visible ? "true" : "false", v->draw_visible ? 4u : 5u};
        if (k == 1 && (strcmp(v->edge_mode, "screen") || c->has_vis1[0])) m[nm++] = (cc_settings_member){"edge_mode", ej, strlen(ej)};
        if (k == 6 && (!v->refill_offscreen || c->has_vis1[3])) m[nm++] = (cc_settings_member){"refill_offscreen", v->refill_offscreen ? "true" : "false", v->refill_offscreen ? 4u : 5u};
        /* GPU1: the lab's renderer, once set or already in the text (sorted: after recent) */
        if (k == 6 && (strcmp(v->renderer, "auto") || c->has_renderer)) m[nm++] = (cc_settings_member){"renderer", rj, strlen(rj)};
        if (k == 6 && (v->still_px != 0 || c->has_vis1[2])) m[nm++] = (cc_settings_member){"still_px", sj, strlen(sj)};
        buf_init(&vals[k]);
        JsonW w;
        jw_init(&w, &vals[k], 0);
        if (k == 5) {
            if (c->has_recent) continue;
            jw_arr_begin(&w);
            for (size_t i = 0; i < s->nrecent_files; i++) jw_str(&w, s->recent_files[i]);
            jw_arr_end(&w);
        } else field_value(&w, v, k);
        if (jw_finish(&w) != JW_OK) rc = -1;
        m[nm++] = (cc_settings_member){FIELDS[k], vals[k].p, vals[k].len};
    }
    struct cc_user_settings *u = s->user_settings;
    if (!rc && s->settings_file && !u->in_mem) {
        if (cc_settings_write(s->pf, s->settings_file, m, nm)) {
            u->in_mem = 1; /* the file would not take it: this session keeps it (the log says why) */
            snprintf(u->note, sizeof u->note, "%s cannot be written: the settings are kept for this session only", s->settings_file);
        }
    }
    if (!rc && (!s->settings_file || u->in_mem)) {
        Buf b;
        buf_init(&b);
        char why[128];
        /* a file that is not a JSON object is left alone, the memory starts from the defaults */
        if (cc_settings_merge(c->text, c->n, m, nm, &b, why) && (buf_free(&b), buf_init(&b), cc_settings_merge(NULL, 0, m, nm, &b, why)))
            rc = -1;
        if (!rc) free(u->mem), u->mem = b.p, u->mem_n = b.len, b.p = NULL;
        buf_free(&b);
    }
    for (size_t k = 0; k < NFIELDS; k++) buf_free(&vals[k]);
    buf_free(&cjb), buf_free(&mjb); /* TESS1 */
    return rc;
}

int cc_settings_action(cc_session *s, const char *name, const char *body, size_t n, Buf *out) {
    int load = !strcmp(name, "settings_load");
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = n ? jr_parse(&a, body, n, &je) : NULL;
    int rc = -1;
    if (!v) {
        say(out, "%s: %s", name, n ? je.msg : load ? "{\"text\": the settings file's text}" : "{field: value, ...}");
        goto end;
    }
    if (!us(s)) goto end;
    if (load) { /* settings_load {"text"}: the page's copy (wasm, localStorage `bimble.settings`) */
        const JrVal *t = field(v, "text");
        if (s->settings_file) say(out, "settings_load: this session's settings are in %s", s->settings_file);
        else if (!t || (t->type != JR_STR && t->type != JR_NULL)) buf_puts(out, "settings_load: {\"text\": the settings file's text}");
        else {
            struct cc_user_settings *u = s->user_settings;
            free(u->mem), u->mem = NULL, u->mem_n = 0, u->note[0] = 0;
            if (t->type == JR_STR && t->n) {
                u->mem = malloc(t->n);
                if (!u->mem) goto end;
                memcpy(u->mem, t->s, t->n), u->mem_n = t->n;
                cc_settings tmp;
                if (cc_settings_parse(t->s, t->n, &tmp)) snprintf(u->note, sizeof u->note, "the stored settings are not JSON: the defaults");
            }
            rc = 0;
        }
        goto end;
    }
    Now c;
    if (now_read(s, &c)) goto end;
    cc_settings next = c.v;
    Buf why;
    buf_init(&why);
    if (cc_settings_set(&next, v, &why)) {
        buf_puts(out, "set_settings: ");
        buf_append(out, why.p, why.len);
    } else rc = write_members(s, &c, &next);
    buf_free(&why);
    now_free(&c);
end:
    arena_free(&a);
    return rc;
}
#else
int cc_settings_trigram(const char *name, char *out, size_t cap) { return (void)name, (void)out, (void)cap, 0; }
void cc_settings_of(const JrVal *v, cc_settings *s) { (void)v, cc_settings_default(s); }
int cc_settings_parse(const char *text, size_t n, cc_settings *s) { return (void)text, (void)n, cc_settings_default(s), 0; }
int cc_settings_set(cc_settings *s, const JrVal *body, Buf *why) { return (void)s, (void)body, buf_puts(why, "view-only"), -1; }
void cc_settings_to_json(const cc_settings *s, char *const *recent, size_t nrecent, Buf *out) { (void)s, (void)recent, (void)nrecent, (void)out; }
int cc_settings_query(cc_session *s, Buf *out) { return (void)s, buf_puts(out, CC_VIEW_ONLY_TEXT), -1; }
int cc_settings_text_query(cc_session *s, Buf *out) { return (void)s, buf_puts(out, CC_VIEW_ONLY_TEXT), -1; }
int cc_settings_action(cc_session *s, const char *name, const char *body, size_t n, Buf *out) {
    return (void)s, (void)name, (void)body, (void)n, buf_puts(out, CC_VIEW_ONLY_TEXT), -1;
}
void cc_settings_user_json(cc_session *s, JsonW *w) { (void)s, jw_null(w); }
void cc_settings_free(cc_session *s) { (void)s; }
void cc_settings_link_opts(cc_session *s, cc_ifc_read_opts *o) { /* TESS1: standard */
    (void)s, o->detail = CC_IFC_DETAIL_STANDARD, o->chord_mm = 2.0, o->max_deg = 18.0;
}
#endif

/* ---- query about (lib.rs:4081 about_ui): in both builds ---- */

#ifndef CC_RELEASE_VERSION
#define CC_RELEASE_VERSION "dev" /* build.sh passes ccore/VERSION */
#endif
#if defined(__wasm__)
#define CC_SYSTEM "browser (WebAssembly)" /* lib.rs:4086 */
#else
#if defined(_WIN32)
#define CC_OS "windows"
#elif defined(__APPLE__)
#define CC_OS "macos"
#elif defined(__linux__)
#define CC_OS "linux"
#else
#define CC_OS "unknown"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define CC_ARCH "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
#define CC_ARCH "x86_64"
#else
#define CC_ARCH "unknown"
#endif
#define CC_SYSTEM CC_OS " " CC_ARCH /* std::env::consts::OS, ARCH */
#endif

/* A string member of the page's body, at most 200 bytes, one line; NULL when absent. */
static const JrVal *about_str(const JrVal *v, const char *key) {
    const JrVal *r = NULL;
    for (size_t i = 0; v && v->type == JR_OBJ && i < v->n; i++)
        if (jr_key_is(&v->members[i], key)) r = &v->members[i].val;
    if (!r || r->type != JR_STR || r->n > 200) return NULL;
    for (size_t i = 0; i < r->n; i++)
        if ((unsigned char)r->s[i] < 0x20) return NULL;
    return r;
}

int cc_about_json(const char *version, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = n ? jr_parse(&a, body, n, &je) : NULL;
    const JrVal *g = about_str(v, "graphics"), *c = about_str(v, "commit");
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "build"), jw_str(&w, CC_RELEASE_VERSION);
    jw_key(&w, "commit");
    if (c) jw_str_n(&w, c->s, c->n);
    else jw_null(&w);
    jw_key(&w, "graphics");
    if (g) jw_str_n(&w, g->s, g->n);
    else jw_null(&w);
    jw_key(&w, "platform"), jw_str(&w, CC_SYSTEM);
    /* lib.rs:4089 the text "Copy for a bug report" copies, with the C core's build and commit */
    Buf t;
    buf_init(&t);
    buf_puts(&t, "Bimble "), buf_puts(&t, version);
    buf_puts(&t, "\nCore: "), buf_puts(&t, CC_RELEASE_VERSION);
    if (c) buf_putc(&t, ' '), buf_append(&t, c->s, c->n);
    buf_puts(&t, "\nGraphics: ");
    if (g) buf_append(&t, g->s, g->n);
    else buf_puts(&t, "unknown");
    buf_puts(&t, "\nSystem: " CC_SYSTEM);
    jw_key(&w, "text"), jw_str_n(&w, t.p ? t.p : "", t.len);
    jw_key(&w, "version"), jw_str(&w, version);
    jw_obj_end(&w);
    int oom = t.oom;
    buf_free(&t);
    arena_free(&a);
    return jw_finish(&w) == JW_OK && !oom ? 0 : (out->oom = 1, -1);
}
