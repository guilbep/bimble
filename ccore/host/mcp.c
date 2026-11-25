/* `bimble-ccore mcp`: the MCP server over stdio (mcp.h), crates/bimble-cli/src/mcp.rs in C.
 * The agent is untrusted input: lines are bounded (MAX_LINE), the JSON reader is bounded
 * (json_r.h: depth, UTF-8), verb arguments go through the verb table's checks, every change goes
 * through the history (validated, undoable). Parameters are data: nothing is executed. */
#define _POSIX_C_SOURCE 200809L
#include "mcp.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "command.h"
#include "file.h"
#include "fmt.h"
#include "history.h"
#include "host.h"
#include "ifc_out.h" /* CC_IFC_VERSION: the C core's own version, as the host's state.version */
#include "json_r.h"
#include "json_w.h"
#include "model.h"
#include "query.h"
#include "verb_table.h"
#include "verbs.h"

/* mcp.rs MAX_LINE: the longest line read; a longer one is refused without being parsed. */
#define MAX_LINE ((size_t)1 << 20)

typedef struct {
    cc_model model;
    cc_history hist;
    const char *path;
    uint64_t rev; /* 1 after the load, + 1 per history change (if_rev, a C addition) */
    int read_only;
} Mcp;

/* ---- reading lines (mcp.rs read_line) ---- */

typedef struct {
    FILE *f;
    char buf[1 << 16];
    size_t at, len;
    int eof;
} Reader;

/* 1: a line in `line` (without its newline) or, when longer than MAX_LINE, *too_long set and
 * *len its length (its bytes read and dropped, never kept). 0 at the end of input. -1 on a read
 * error. */
static int read_line(Reader *r, Buf *line, int *too_long, size_t *len) {
    buf_clear(line);
    *too_long = 0;
    *len = 0;
    for (;;) {
        if (r->at == r->len) {
            if (r->eof) return *len > 0;
            r->len = fread(r->buf, 1, sizeof r->buf, r->f);
            r->at = 0;
            if (r->len == 0) {
                if (ferror(r->f)) return -1;
                r->eof = 1;
                return *len > 0;
            }
        }
        const char *p = r->buf + r->at;
        size_t avail = r->len - r->at;
        const char *nl = memchr(p, '\n', avail);
        size_t part = nl ? (size_t)(nl - p) : avail;
        *len += part;
        if (*len <= MAX_LINE) buf_append(line, p, part);
        else if (!*too_long) *too_long = 1, buf_clear(line);
        r->at += part + (nl ? 1 : 0);
        if (nl) return 1;
    }
}

/* u8::is_ascii_whitespace: space, \t, \n, \x0c, \r. */
static int blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\x0c' && s[i] != '\r') return 0;
    return 1;
}

/* ---- serde_json's view of a JSON line ---- */

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* serde's 4 hex digits of a \u escape at s[i..): -1 with the error ("EOF while parsing a string" at
 * the end, "invalid escape" after the 4 digits), else the value. */
static long hex4(const char *s, size_t n, size_t i, const char **msg, size_t *col) {
    if (i + 4 > n) return *msg = "EOF while parsing a string", *col = n, -1;
    long v = 0;
    for (int k = 0; k < 4; k++) {
        int d = hexval(s[i + k]);
        if (d < 0) return *msg = "invalid escape", *col = i + 4, -1;
        v = v * 16 + d;
    }
    return v;
}

/* serde_json's SliceRead::parse_str from the opening quote at s[q]: it reads in order, so its first
 * error may not be the C reader's (which finds the closing quote first, then decodes). 1 with
 * serde's error and column, 0 when the string is fine. */
static int serde_string(const char *s, size_t n, size_t q, const char **msg, size_t *col) {
    for (size_t i = q + 1;;) {
        if (i >= n) return *msg = "EOF while parsing a string", *col = n, 1;
        unsigned char c = (unsigned char)s[i];
        if (c == '"') return 0;
        if (c < 0x20) return *msg = "control character (\\u0000-\\u001F) found while parsing a string", *col = i + 1, 1;
        if (c != '\\') {
            i++;
            continue;
        }
        if (++i >= n) return *msg = "EOF while parsing a string", *col = n, 1;
        char e = s[i++];
        if (strchr("\"\\/bfnrt", e) && e) continue;
        if (e != 'u') return *msg = "invalid escape", *col = i, 1;
        long cp = hex4(s, n, i, msg, col);
        if (cp < 0) return 1;
        i += 4;
        if (cp >= 0xDC00 && cp <= 0xDFFF) return *msg = "lone leading surrogate in hex escape", *col = i, 1;
        if (cp < 0xD800 || cp > 0xDBFF) continue;
        if (i >= n) return *msg = "EOF while parsing a string", *col = n, 1;
        if (s[i++] != '\\') return *msg = "unexpected end of hex escape", *col = i, 1;
        if (i >= n) return *msg = "EOF while parsing a string", *col = n, 1;
        if (s[i++] != 'u') return *msg = "unexpected end of hex escape", *col = i, 1;
        long lo = hex4(s, n, i, msg, col);
        if (lo < 0) return 1;
        i += 4;
        if (lo < 0xDC00 || lo > 0xDFFF) return *msg = "lone leading surrogate in hex escape", *col = i, 1;
    }
}

/* serde_json::from_slice's error text for a line jr_parse refused: "<what> at line 1 column C"
 * (a line has no '\n', so line 1). The C reader's message and offset are mapped to serde's: an
 * error seen on a peeked byte is at column offset + 1, one at the end of input at column n;
 * a bad literal (`tru`, `nul x`) is serde's "expected ident" after the first byte that differs;
 * the end of input after `[` or a `,`, or in a number, is "EOF while parsing a list" / "… a value";
 * a digit after a leading 0 is "invalid number"; an error in a string is the first one serde's
 * in-order read meets. Not the same (golden/DEVIATIONS.md "MCP"): nesting deeper than 64 (json_r's
 * budget; serde: 128) and bad UTF-8 (json_r checks the whole line first). */
static void parse_error_text(const char *s, size_t n, const JrError *je, Buf *out) {
    const char *msg = je->msg;
    size_t off = je->offset, col = off + 1;
    int in_string = !strcmp(msg, "EOF while parsing a string") || !strcmp(msg, "invalid escape") ||
                    !strncmp(msg, "control character", 17) || !strcmp(msg, "lone leading surrogate in hex escape") ||
                    !strcmp(msg, "unexpected end of hex escape");
    if (in_string) {
        /* The string's opening quote: the line is valid JSON up to the error, so quotes outside
         * strings open them, and inside a string a quote not escaped closes it. */
        size_t q = 0, open = 0;
        for (size_t i = 0; i < n && i <= off; i++) {
            if (!open && s[i] == '"') open = 1, q = i;
            else if (open && s[i] == '\\') i++;
            else if (open && s[i] == '"') open = 0;
        }
        if (!serde_string(s, n, q, &msg, &col)) col = off + 1; /* not met: the C text */
    } else if (off >= n) {
        col = n;
        size_t k = n; /* the last byte that is not whitespace */
        while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\r')) k--;
        if (!strcmp(msg, "invalid number") || (k > 0 && s[k - 1] == ',')) msg = "EOF while parsing a value";
        else if (k > 0 && s[k - 1] == '[') msg = "EOF while parsing a list";
    } else if (off > 0 && s[off] >= '0' && s[off] <= '9' && s[off - 1] == '0') {
        msg = "invalid number"; /* a digit after a leading 0 (C: the number ended at the 0) */
    } else if (!strcmp(msg, "expected value") && (s[off] == 't' || s[off] == 'f' || s[off] == 'n')) {
        const char *w = s[off] == 't' ? "true" : s[off] == 'f' ? "false" : "null";
        size_t k = 1;
        while (w[k] && off + k < n && s[off + k] == w[k]) k++;
        if (off + k >= n) msg = "EOF while parsing a value", col = n;
        else msg = "expected ident", col = off + k + 1;
    }
    buf_puts(out, "parse error: ");
    buf_puts(out, msg);
    buf_puts(out, " at line 1 column ");
    buf_u64(out, col);
}

/* serde_json refuses a number out of f64's range ("number out of range"); the C reader gives an
 * infinity. The first such number, depth first, or NULL. */
static const JrVal *out_of_range(const JrVal *v) {
    if (v->type == JR_NUM) return v->num - v->num != 0 ? v : NULL; /* inf - inf is NaN */
    size_t n = v->type == JR_ARR || v->type == JR_OBJ ? v->n : 0;
    for (size_t i = 0; i < n; i++) {
        const JrVal *x = out_of_range(v->type == JR_ARR ? &v->items[i] : &v->members[i].val);
        if (x) return x;
    }
    return NULL;
}

/* Value::get on an object: serde's map keeps the last of duplicate keys. NULL when absent or
 * when `v` is not an object. */
static const JrVal *get(const JrVal *v, const char *key) {
    if (!v || v->type != JR_OBJ) return NULL;
    const JrVal *r = NULL;
    for (size_t i = 0; i < v->n; i++)
        if (jr_key_is(&v->members[i], key)) r = &v->members[i].val;
    return r;
}

static int key_cmp(const JrMember *a, const JrMember *b) {
    size_t n = a->key_n < b->key_n ? a->key_n : b->key_n;
    int c = memcmp(a->key, b->key, n);
    if (c) return c;
    return a->key_n < b->key_n ? -1 : a->key_n > b->key_n;
}

static int member_ptr_cmp(const void *x, const void *y) {
    const JrMember *a = *(const JrMember *const *)x, *b = *(const JrMember *const *)y;
    int c = key_cmp(a, b);
    return c ? c : (a < b ? -1 : a > b); /* document order among duplicates */
}

/* The members of an object as serde's Map holds them: one per key (the last), sorted by key
 * bytes (BTreeMap). `*out` is malloc'd (NULL for none); returns the count, or -1 out of memory. */
static long map_members(const JrVal *v, const JrMember ***out) {
    *out = NULL;
    if (!v->n) return 0;
    const JrMember **p = malloc(v->n * sizeof *p);
    if (!p) return -1;
    for (size_t i = 0; i < v->n; i++) p[i] = &v->members[i];
    qsort(p, v->n, sizeof *p, member_ptr_cmp);
    size_t k = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (i + 1 < v->n && key_cmp(p[i], p[i + 1]) == 0) continue; /* a later duplicate follows */
        p[k++] = p[i];
    }
    *out = p;
    return (long)k;
}

/* A number as serde_json reads and prints it: an integer token is a u64 or an i64 when it fits,
 * else an f64 ("-0" is the f64 -0.0); f64 as serde prints it (1500.0, 1e+30). */
static void write_number(Buf *b, const JrVal *v) {
    if (v->num_int) {
        const char *d = v->s + v->num_neg;
        size_t nd = v->n - (size_t)v->num_neg;
        static const char U64MAX[] = "18446744073709551615", I64MIN[] = "9223372036854775808";
        size_t lim = v->num_neg ? sizeof I64MIN - 1 : sizeof U64MAX - 1;
        const char *max = v->num_neg ? I64MIN : U64MAX;
        int zero = nd == 1 && d[0] == '0';
        int fits = nd < lim || (nd == lim && memcmp(d, max, lim) <= 0);
        if (fits && !(zero && v->num_neg)) {
            buf_append(b, v->s, v->n);
            return;
        }
    }
    fmt_f64_json(b, v->num);
}

/* serde_json::Value::to_string: compact, object keys sorted and unique. -1 out of memory. */
static int write_value(Buf *b, const JrVal *v) {
    switch (v->type) {
    case JR_NULL: buf_puts(b, "null"); return 0;
    case JR_BOOL: buf_puts(b, v->b ? "true" : "false"); return 0;
    case JR_NUM: write_number(b, v); return 0;
    case JR_STR: json_escape(b, v->s, v->n); return 0;
    case JR_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            if (write_value(b, &v->items[i])) return -1;
        }
        buf_putc(b, ']');
        return 0;
    default: {
        const JrMember **m;
        long k = map_members(v, &m);
        if (k < 0) return -1;
        buf_putc(b, '{');
        int rc = 0;
        for (long i = 0; i < k && !rc; i++) {
            if (i) buf_putc(b, ',');
            json_escape(b, m[i]->key, m[i]->key_n);
            buf_putc(b, ':');
            rc = write_value(b, &m[i]->val);
        }
        buf_putc(b, '}');
        free(m);
        return rc;
    }
    }
}

/* ---- the tools (mcp.rs `tools` and `call`) ---- */

#define NO_ARGS "{\"additionalProperties\":false,\"properties\":{},\"type\":\"object\"}"
#define NAMED(what)                                                                                          \
    "{\"additionalProperties\":false,\"properties\":{\"args\":{\"description\":\"The arguments, as "         \
    "`describe_verb` gives for a verb\",\"type\":\"object\"},\"name\":{\"description\":\"" what "\",\"type\":" \
    "\"string\"}},\"required\":[\"name\"],\"type\":\"object\"}"
/* serde_json prints json!'s objects with sorted keys: description, inputSchema, name. */
static const char TOOLS[] =
    "{\"tools\":["
    "{\"description\":\"The verbs that edit the model (place, move, copy, delete), with a summary each.\","
    "\"inputSchema\":" NO_ARGS ",\"name\":\"list_verbs\"},"
    "{\"description\":\"JSON schema of a verb's arguments.\",\"inputSchema\":{\"additionalProperties\":false,"
    "\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],\"type\":\"object\"},"
    "\"name\":\"describe_verb\"},"
    "{\"description\":\"Run a verb on the model (one undoable step). Lengths in mm, storey-relative. Not saved "
    "until `save`.\",\"inputSchema\":" NAMED("Verb name, from list_verbs") ",\"name\":\"run_verb\"},"
    "{\"description\":\"Read the model: storeys, materials, elements (filter), element ({\\\"id\\\"}).\","
    "\"inputSchema\":" NAMED("storeys, materials, elements or element") ",\"name\":\"query\"},"
    "{\"description\":\"Undo the last step.\",\"inputSchema\":" NO_ARGS ",\"name\":\"undo\"},"
    "{\"description\":\"Redo the last undone step.\",\"inputSchema\":" NO_ARGS ",\"name\":\"redo\"},"
    "{\"description\":\"Write the model to its file.\",\"inputSchema\":" NO_ARGS ",\"name\":\"save\"}"
    "]}";

static const char READ_ONLY[] = "read only: the server was started with --read-only";

/* mcp.rs `fields`: an object whose keys are all in `allowed` (NULL-terminated). The first unknown
 * key in sorted order is named, as serde's Map iterates. 0 ok, -1 with the text in `err`. */
static int fields(const JrVal *args, const char *const *allowed, Buf *err) {
    if (args->type != JR_OBJ) {
        buf_puts(err, "arguments must be an object");
        return -1;
    }
    const JrMember **m;
    long k = map_members(args, &m);
    if (k < 0) {
        buf_puts(err, "out of memory");
        return -1;
    }
    for (long i = 0; i < k; i++) {
        int ok = 0;
        for (const char *const *a = allowed; *a && !ok; a++) ok = jr_key_is(m[i], *a);
        if (!ok) {
            buf_puts(err, "unknown argument ");
            cc_buf_debug_str(err, m[i]->key, m[i]->key_n);
            free(m);
            return -1;
        }
    }
    free(m);
    return 0;
}

/* mcp.rs `name_arg`: the "name" string (the reader puts a NUL after each string; a name with a
 * NUL inside names nothing, the callers check its length), or NULL with the text in `err`. */
static const JrVal *name_arg(const JrVal *o, Buf *err) {
    const JrVal *v = get(o, "name");
    if (!v || v->type != JR_STR) {
        buf_puts(err, "name (a string) is required");
        return NULL;
    }
    return v;
}

static const char *const NONE[] = {NULL};
static const char *const NAME[] = {"name", NULL};
static const char *const NAME_ARGS[] = {"name", "args", NULL};

/* A verb of the table by name (Rust's or a C addition), NULL when none. */
static const cc_verb_def *find_verb(const JrVal *name) {
    if (strlen(name->s) != name->n) return NULL;
    return cc_verb_find(name->s);
}

/* The verb's arguments as text (serde's Value, so sorted and without duplicates), with the two C
 * additions "dry_run" and "if_rev" taken out of an object (calls.h "verb"). -1 with `err`. */
static int verb_args(const JrVal *args, Buf *text, int *dry_run, int64_t *if_rev, Buf *err) {
    *dry_run = 0, *if_rev = -1;
    if (!args) return 0; /* none: null */
    if (args->type == JR_OBJ) {
        const JrVal *d = get(args, "dry_run"), *r = get(args, "if_rev");
        if (d && d->type != JR_BOOL) {
            buf_puts(err, "bad arguments: dry_run: expected a boolean");
            return -1;
        }
        if (r && (r->type != JR_NUM || !r->num_int || r->num_neg || r->n > 18)) {
            buf_puts(err, "bad arguments: if_rev: expected a non-negative integer");
            return -1;
        }
        if (d) *dry_run = d->b;
        if (r) {
            *if_rev = 0;
            for (size_t k = 0; k < r->n; k++) *if_rev = *if_rev * 10 + (r->s[k] - '0');
        }
        if (d || r) {
            const JrMember **m;
            long k = map_members(args, &m);
            if (k < 0) {
                buf_puts(err, "out of memory");
                return -1;
            }
            buf_putc(text, '{');
            int first = 1, rc = 0;
            for (long i = 0; i < k && !rc; i++) {
                if (jr_key_is(m[i], "dry_run") || jr_key_is(m[i], "if_rev")) continue;
                if (!first) buf_putc(text, ',');
                first = 0;
                json_escape(text, m[i]->key, m[i]->key_n);
                buf_putc(text, ':');
                rc = write_value(text, &m[i]->val);
            }
            buf_putc(text, '}');
            free(m);
            if (rc || text->oom) {
                buf_puts(err, "out of memory");
                return -1;
            }
            return 0;
        }
    }
    if (write_value(text, args) || text->oom) {
        buf_puts(err, "out of memory");
        return -1;
    }
    return 0;
}

static int run_verb(Mcp *s, const JrVal *args, Buf *out) {
    if (fields(args, NAME_ARGS, out)) return -1;
    const JrVal *name = name_arg(args, out);
    if (!name) return -1;
    Buf text;
    buf_init(&text);
    int dry_run, rc = -1;
    int64_t if_rev;
    if (verb_args(get(args, "args"), &text, &dry_run, &if_rev, out)) goto done;
    if (s->read_only && !dry_run) {
        buf_puts(out, READ_ONLY);
        goto done;
    }
    cc_cmds l;
    cc_cmds_init(&l);
    if (strlen(name->s) != name->n) { /* a NUL inside: no verb's name (dispatch::run's text) */
        buf_puts(out, "unknown verb ");
        cc_buf_debug_str(out, name->s, name->n);
        buf_puts(out, "; known: ");
        for (size_t i = 0; i < cc_verb_count; i++) {
            if (i) buf_puts(out, ", ");
            buf_puts(out, cc_verb_table[i].name);
        }
    } else if (cc_verb_run_ex(&s->model, name->s, text.p ? text.p : "", text.len, dry_run, if_rev, s->rev, &l, out) == 0) {
        if (dry_run) { /* {"commands":[…],"rev":N} (a C addition) */
            buf_puts(out, "{\"commands\":");
            cc_commands_json(l.v, l.n, out);
            buf_puts(out, ",\"rev\":");
            buf_u64(out, s->rev);
            buf_putc(out, '}');
            rc = 0;
        } else {
            Buf added;
            buf_init(&added);
            for (size_t i = 0; i < l.n; i++)
                if (l.v[i].op == CC_CMD_ADD_ELEMENT) {
                    if (added.len) buf_putc(&added, ',');
                    json_escape(&added, l.v[i].element.id.c, 22);
                }
            if (cc_history_apply(&s->hist, &s->model, l.v, l.n, out) == 0) { /* History::apply */
                s->rev++;
                buf_puts(out, "{\"added\":[");
                buf_append(out, added.p, added.len);
                buf_puts(out, "],\"commands\":");
                buf_u64(out, l.n);
                buf_putc(out, '}');
                rc = 0;
            }
            buf_free(&added);
        }
    }
    cc_cmds_free(&l);
done:
    buf_free(&text);
    return rc;
}

/* dispatch::query's names: the C core's own queries (verbs, describe, …) are not offered here. */
static const char *const QUERIES[] = {"storeys", "materials", "elements", "element",
                                      "project", "views",     "sheets",   "schedules"};

static int query(Mcp *s, const JrVal *args, Buf *out) {
    if (fields(args, NAME_ARGS, out)) return -1;
    const JrVal *name = name_arg(args, out);
    if (!name) return -1;
    int known = 0;
    for (size_t i = 0; i < sizeof QUERIES / sizeof *QUERIES && !known; i++)
        known = strlen(QUERIES[i]) == name->n && !memcmp(QUERIES[i], name->s, name->n);
    if (!known) {
        buf_puts(out, "unknown query ");
        cc_buf_debug_str(out, name->s, name->n);
        buf_puts(out, "; known: storeys, materials, elements, element, project, views, sheets, schedules");
        return -1;
    }
    Buf text;
    buf_init(&text);
    const JrVal *a = get(args, "args");
    int rc = -1;
    if (a && (write_value(&text, a) || text.oom)) buf_puts(out, "out of memory");
    else rc = cc_query(&s->model, name->s, text.p ? text.p : "", text.len, out) == 200 ? 0 : -1;
    buf_free(&text);
    return rc;
}

/* The file name of `path` (Path::file_name, lossy): after the last '/', trailing '/' dropped. */
static void file_name(const char *path, Buf *out) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    if (n - i == 2 && !memcmp(path + i, "..", 2)) return; /* None */
    buf_append(out, path + i, n - i);
}

/* file::save's choice (file.rs:145): Path::extension() == "json", so ".json" alone is binary. */
static int json_extension(const char *path) {
    Buf name;
    buf_init(&name);
    file_name(path, &name);
    /* the extension starts at the last '.', which must not be the name's first byte */
    int r = name.len > 5 && !memcmp(name.p + name.len - 5, ".json", 5);
    buf_free(&name);
    return r;
}

static int save(Mcp *s, Buf *out) {
    Buf b;
    buf_init(&b);
    int form = json_extension(s->path);
    int ok = (form ? cc_model_save(&s->model, &b) : cc_model_save_v2(&s->model, &b)) == 0 && !b.oom;
    const cc_platform *pf = cc_platform_native();
    if (ok) ok = pf->write_file(pf->ctx, s->path, b.p, b.len) == 0; /* a temp file, then rename */
    buf_free(&b);
    if (!ok) {
        buf_puts(out, s->path);
        buf_puts(out, ": cannot write");
        return -1;
    }
    buf_puts(out, "saved ");
    buf_u64(out, s->model.nelements);
    buf_puts(out, " elements to ");
    file_name(s->path, out);
    return 0;
}

static int undo_redo(Mcp *s, int undo, Buf *out) {
    const char *tool = undo ? "undo" : "redo";
    size_t n = undo ? s->hist.nundo : s->hist.nredo;
    if (!n) {
        buf_puts(out, "nothing to ");
        buf_puts(out, tool);
        return 0;
    }
    const char *label = (undo ? s->hist.undo : s->hist.redo)[n - 1].label;
    Buf l; /* the label, kept: the step moves to the other stack */
    buf_init(&l);
    buf_puts(&l, label);
    int done = undo ? cc_history_undo(&s->hist, &s->model) : cc_history_redo(&s->hist, &s->model);
    if (done) {
        s->rev++;
        buf_puts(out, tool);
        buf_puts(out, ": ");
        buf_append(out, l.p, l.len);
    } else {
        buf_puts(out, "nothing to ");
        buf_puts(out, tool);
    }
    buf_free(&l);
    return 0;
}

/* mcp.rs `call`: 0 with the tool's text in `out`, -1 with the error text. */
static int call(Mcp *s, const JrVal *tool, const JrVal *args, Buf *out) {
    /* params.name as a str, else "" (a NUL inside: no tool's name) */
    const char *name = tool && tool->type == JR_STR ? tool->s : "";
    size_t tn = tool && tool->type == JR_STR ? tool->n : 0;
    const char *t = strlen(name) == tn ? name : "";
    if (!strcmp(t, "list_verbs")) {
        if (fields(args, NONE, out)) return -1;
        cc_verbs_list_json(out);
        return 0;
    }
    if (!strcmp(t, "describe_verb")) {
        if (fields(args, NAME, out)) return -1;
        const JrVal *name = name_arg(args, out);
        if (!name) return -1;
        const cc_verb_def *v = find_verb(name);
        if (!v) {
            buf_puts(out, "unknown verb ");
            cc_buf_debug_str(out, name->s, name->n);
            return -1;
        }
        buf_puts(out, v->schema);
        return 0;
    }
    if (!strcmp(t, "run_verb")) return run_verb(s, args, out);
    if (!strcmp(t, "query")) return query(s, args, out);
    if (!strcmp(t, "undo") || !strcmp(t, "redo")) {
        if (fields(args, NONE, out)) return -1;
        if (s->read_only) {
            buf_puts(out, READ_ONLY);
            return -1;
        }
        return undo_redo(s, !strcmp(t, "undo"), out);
    }
    if (!strcmp(t, "save")) {
        if (fields(args, NONE, out)) return -1;
        if (s->read_only) {
            buf_puts(out, READ_ONLY);
            return -1;
        }
        return save(s, out);
    }
    buf_puts(out, "unknown tool ");
    cc_buf_debug_str(out, name, tn);
    buf_puts(out, "; known: list_verbs, describe_verb, run_verb, query, undo, redo, save");
    return -1;
}

/* ---- JSON-RPC (mcp.rs `handle`, `error`) ---- */

/* {"error":{"code":C,"message":M},"id":ID,"jsonrpc":"2.0"} (json! sorts the keys). */
static void error_reply(Buf *b, const JrVal *id, int code, const char *msg, size_t n) {
    buf_puts(b, "{\"error\":{\"code\":");
    buf_i64(b, code);
    buf_puts(b, ",\"message\":");
    json_escape(b, msg, n);
    buf_puts(b, "},\"id\":");
    if (id) write_value(b, id);
    else buf_puts(b, "null");
    buf_puts(b, ",\"jsonrpc\":\"2.0\"}");
}

/* The reply to one message in `b`; 0 when there is none (a notification: no "id"). */
static int handle(Mcp *s, const JrVal *msg, Buf *b) {
    const JrVal *id = get(msg, "id");
    if (!id) return 0;
    const JrVal *params = get(msg, "params");
    const JrVal *method = get(msg, "method");
    if (!method || method->type != JR_STR) {
        error_reply(b, id, -32600, "no method", 9);
        return 1;
    }
    const char *m = method->s;
    int known = strlen(m) == method->n;
    Buf result;
    buf_init(&result);
    if (known && !strcmp(m, "initialize")) {
        const JrVal *pv = get(params, "protocolVersion");
        buf_puts(&result, "{\"capabilities\":{\"tools\":{}},\"protocolVersion\":");
        if (pv && pv->type == JR_STR) json_escape(&result, pv->s, pv->n);
        else buf_puts(&result, "\"2025-06-18\"");
        buf_puts(&result, ",\"serverInfo\":{\"name\":\"bimble\",\"version\":\"" CC_IFC_VERSION "\"}}");
    } else if (known && !strcmp(m, "ping")) {
        buf_puts(&result, "{}");
    } else if (known && !strcmp(m, "tools/list")) {
        buf_puts(&result, TOOLS);
    } else if (known && !strcmp(m, "tools/call")) {
        static const JrVal empty = {.type = JR_OBJ};
        const JrVal *args = get(params, "arguments");
        if (!args) args = &empty;
        Buf text;
        buf_init(&text);
        int err = call(s, get(params, "name"), args, &text) != 0;
        if (text.oom) {
            buf_clear(&text);
            text.oom = 0;
            buf_puts(&text, "out of memory");
            err = 1;
        }
        buf_puts(&result, "{\"content\":[{\"text\":");
        json_escape(&result, text.p ? text.p : "", text.len);
        buf_puts(&result, ",\"type\":\"text\"}],\"isError\":");
        buf_puts(&result, err ? "true}" : "false}");
        buf_free(&text);
    } else {
        Buf e;
        buf_init(&e);
        buf_puts(&e, "method not found: ");
        buf_append(&e, method->s, method->n);
        error_reply(b, id, -32601, e.p, e.len);
        buf_free(&e);
        buf_free(&result);
        return 1;
    }
    buf_puts(b, "{\"id\":");
    write_value(b, id);
    buf_puts(b, ",\"jsonrpc\":\"2.0\",\"result\":");
    buf_append(b, result.p, result.len);
    buf_putc(b, '}');
    buf_free(&result);
    return 1;
}

/* One line: the reply in `b` (1) or none (0). */
static int line_reply(Mcp *s, const char *p, size_t n, Buf *b) {
    if (blank(p, n)) return 0;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *msg = jr_parse(&a, p, n, &je);
    const JrVal *bad = msg ? out_of_range(msg) : NULL;
    int r = 1;
    if (!msg || bad) {
        Buf e;
        buf_init(&e);
        if (msg) {
            buf_puts(&e, "parse error: number out of range at line 1 column ");
            buf_u64(&e, bad->off + bad->n);
        } else {
            parse_error_text(p, n, &je, &e);
        }
        error_reply(b, NULL, -32700, e.p, e.len);
        buf_free(&e);
    } else {
        r = handle(s, msg, b);
    }
    arena_free(&a);
    return r;
}

static int usage(void) {
    fprintf(stderr, "usage: bimble-ccore mcp [--read-only] <model.bimble>\n");
    return 2;
}

int cc_mcp_main(int argc, char **argv) {
    Mcp s;
    memset(&s, 0, sizeof s);
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--read-only")) s.read_only = 1;
        else {
            fprintf(stderr, "bimble-ccore mcp: unknown flag %s\n", argv[i]);
            return usage();
        }
    }
    if (argc - i != 1) return usage();
    s.path = argv[i];
    const cc_platform *pf = cc_platform_native();
    char *text = NULL;
    size_t n = 0;
    int st = pf->read_file(pf->ctx, s.path, CC_MAX_FILE_BYTES, &text, &n);
    if (st != CC_PF_OK) {
        fprintf(stderr, "bimble-ccore mcp: %s: %s\n", s.path,
                st == CC_PF_NOT_FOUND ? "cannot read" : st == CC_PF_TOO_LARGE ? "file too large" : "read error");
        return 1;
    }
    cc_error err;
    int bad = cc_model_load(&s.model, text, n, &err);
    free(text);
    if (bad) {
        fprintf(stderr, "bimble-ccore mcp: %s: %s\n", s.path, err.msg);
        cc_model_free(&s.model);
        return 1;
    }
    cc_history_init(&s.hist);
    s.rev = 1;
    signal(SIGPIPE, SIG_IGN); /* a closed stdout is a write error, as in Rust */
    fprintf(stderr, "bimble-ccore mcp: serving %s%s\n", s.path, s.read_only ? " (read only)" : "");

    static Reader r;
    r.f = stdin;
    Buf line, out;
    buf_init(&line);
    buf_init(&out);
    int rc = 0, too_long, got;
    size_t len;
    while ((got = read_line(&r, &line, &too_long, &len)) == 1) {
        buf_clear(&out);
        int has = 1;
        if (line.oom) { /* out of memory is an error value: one reply that says so */
            line.oom = 0;
            error_reply(&out, NULL, -32603, "out of memory", 13);
        } else if (too_long) {
            char m[96];
            snprintf(m, sizeof m, "line of %zu+ bytes: at most %zu", len, MAX_LINE);
            error_reply(&out, NULL, -32600, m, strlen(m));
        } else {
            has = line_reply(&s, line.p ? line.p : "", line.len, &out);
        }
        if (out.oom) {
            out.oom = 0;
            buf_clear(&out);
            error_reply(&out, NULL, -32603, "out of memory", 13);
            has = 1;
        }
        if (has) {
            buf_putc(&out, '\n');
            if (fwrite(out.p, 1, out.len, stdout) != out.len || fflush(stdout)) {
                fprintf(stderr, "bimble-ccore mcp: stdout: write error\n");
                rc = 1;
                break;
            }
        }
    }
    if (got < 0) {
        fprintf(stderr, "bimble-ccore mcp: stdin: read error\n");
        rc = 1;
    }
    buf_free(&line);
    buf_free(&out);
    cc_history_free(&s.hist);
    cc_model_free(&s.model);
    return rc;
}
