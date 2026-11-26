/* The file dialog's queries (ticket FS1): fs.h, the path rule. Through the native cc_platform
 * (list_dir, stat_path, real_path): no #ifdef per system here. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* mkdtemp */
#endif
#include "fs.h"
#include "fs_rule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "arena.h"
#include "calls.h"
#include "host.h"
#include "json_r.h"
#include "json_w.h"

#define FS_MAX_PATH 4096
#define FS_LIST_BYTES ((size_t)16 << 20) /* the names of one folder, all together */

static char *roots[FS_MAX_ROOTS];       /* resolved (realpath) */
static char *roots_given[FS_MAX_ROOTS]; /* as given, normalised by fs_rule.h; NULL: not absolute */
static int nroots, home_root = -1;

/* The folder listed last per purpose ("open", "save_as"…): the dialog starts there next time.
 * The host's memory: it lasts as long as the process. */
#define FS_PURPOSES 16
static struct {
    char purpose[32];
    char *dir;
} last[FS_PURPOSES];

static int purpose_slot(const JrVal *p, int make) {
    if (!p || p->type != JR_STR || !p->n || p->n >= sizeof last[0].purpose || strlen(p->s) != p->n) return -1;
    for (int i = 0; i < FS_PURPOSES; i++)
        if (last[i].dir && !strcmp(last[i].purpose, p->s)) return i;
    if (!make) return -1;
    for (int i = 0; i < FS_PURPOSES; i++)
        if (!last[i].dir) {
            memcpy(last[i].purpose, p->s, p->n + 1);
            return i;
        }
    return -1;
}

static void remember(const JrVal *p, const char *dir) {
    int i = purpose_slot(p, 1);
    if (i < 0) return;
    char *c = malloc(strlen(dir) + 1);
    if (!c) return;
    strcpy(c, dir);
    free(last[i].dir);
    last[i].dir = c;
}

#ifdef _WIN32
#define FSR_WIN 1
#else
#define FSR_WIN 0
#endif

/* The resolved path `p` is a root or under one (fs_rule.h). */
static int in_roots(const char *p) {
    for (int i = 0; i < nroots; i++)
        if (fsr_under(p, roots[i], FSR_WIN)) return 1;
    return 0;
}

/* The normalised (not resolved) path `p` is under a root as resolved or as given: the lexical
 * check, before the file system is touched (fs_rule.h). */
static int in_roots_lexical(const char *p) {
    for (int i = 0; i < nroots; i++)
        if (fsr_under(p, roots[i], FSR_WIN) || (roots_given[i] && fsr_under(p, roots_given[i], FSR_WIN))) return 1;
    return 0;
}

/* A root is a network path ("//server/share"): only then may fs_list take one. */
static int unc_root(void) {
    for (int i = 0; i < nroots; i++)
        if (roots[i][0] == '/' && roots[i][1] == '/') return 1;
    return 0;
}

int cc_host_fs_add_root(const char *dir) {
    const cc_platform *pf = cc_platform_native();
    char r[FS_MAX_PATH + 1], g[FS_MAX_PATH + 2];
    cc_pf_stat st;
    /* a device path is never a root; a relative root is resolved, but has no lexical form */
    int lx = dir && *dir ? fsr_normalise(dir, FSR_WIN, g, sizeof g) : FSR_BAD;
    if (nroots >= FS_MAX_ROOTS || !dir || !*dir || lx == FSR_DEVICE || pf->real_path(pf->ctx, dir, r, sizeof r) ||
        pf->stat_path(pf->ctx, r, 1, &st) || st.kind != CC_PF_DIR)
        return -1;
    for (int i = 0; i < nroots; i++)
        if (!strcmp(roots[i], r)) return 0;
    char *c = malloc(strlen(r) + 1), *cg = lx == FSR_BAD ? NULL : malloc(strlen(g) + 1);
    if (!c || (lx != FSR_BAD && !cg)) return free(c), free(cg), -1;
    strcpy(c, r);
    if (cg) strcpy(cg, g);
    roots_given[nroots] = cg;
    roots[nroots++] = c;
    return 0;
}

void cc_host_fs_init(void) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    if (h && *h && home_root < 0 && !cc_host_fs_add_root(h)) home_root = nroots - 1;
}

/* ---- fs_list ---- */

typedef struct {
    const char *name; /* into the list_dir block */
    size_t n;
} Name;

static int name_cmp(const void *a, const void *b) {
    const Name *x = a, *y = b;
    size_t k = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->name, y->name, k);
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

/* `name` ends with one of ext[0..next) (".ifc"), without case. */
static int ext_ok(const char *name, size_t n, const JrVal *ext) {
    if (!ext) return 1;
    for (size_t i = 0; i < ext->n; i++) {
        const JrVal *e = &ext->items[i];
        if (e->n > n) continue;
        size_t j = 0;
        for (; j < e->n; j++) {
            char a = name[n - e->n + j], b = e->s[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == e->n) return 1;
    }
    return 0;
}

static const char *kind_text(int k) {
    return k == CC_PF_DIR ? "dir" : k == CC_PF_FILE ? "file" : k == CC_PF_LINK ? "link" : "other";
}

static int fs_err(Buf *out, const char *msg) {
    buf_puts(out, "fs_list: ");
    buf_puts(out, msg);
    return CC_ERR;
}

static int fs_list(const char *body, size_t n, Buf *out) {
    const cc_platform *pf = cc_platform_native();
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *d = v && v->type == JR_OBJ ? jr_get(v, "dir") : NULL;
    const JrVal *ext = v && v->type == JR_OBJ ? jr_get(v, "ext") : NULL;
    int rc = CC_ERR;
    char *names = NULL;
    Name *list = NULL;
    if (ext && ext->type == JR_NULL) ext = NULL;
    /* FIXDOC: the lexical rule first (fs_rule.h): nothing outside the roots reaches the file system */
    char nd[FS_MAX_PATH + 2];
    int lx = d && d->type == JR_STR && strlen(d->s) == d->n ? fsr_normalise(d->s, FSR_WIN, nd, sizeof nd) : FSR_BAD;
    if (lx == FSR_BAD || !d->n || d->n > FS_MAX_PATH) {
        rc = fs_err(out, "dir: an absolute path, without NUL, at most 4096 bytes");
        goto end;
    }
    if (lx == FSR_DEVICE || (lx == FSR_UNC && !unc_root())) {
        rc = fs_err(out, FS_NETWORK);
        goto end;
    }
    if (!in_roots_lexical(nd)) {
        rc = fs_err(out, FS_OUTSIDE);
        goto end;
    }
    if (ext && (ext->type != JR_ARR || ext->n > 16)) {
        rc = fs_err(out, "ext: at most 16 strings, as \".ifc\"");
        goto end;
    }
    for (size_t i = 0; ext && i < ext->n; i++)
        if (ext->items[i].type != JR_STR || !ext->items[i].n || ext->items[i].n > 16) {
            rc = fs_err(out, "ext: at most 16 strings, as \".ifc\"");
            goto end;
        }
    char rd[FS_MAX_PATH + 1];
    cc_pf_stat st;
    /* then resolved: "..", "." and every link, and the rule again */
    if (pf->real_path(pf->ctx, d->s, rd, sizeof rd)) {
        rc = fs_err(out, "no such folder");
        goto end;
    }
    if (!in_roots(rd)) {
        rc = fs_err(out, FS_OUTSIDE);
        goto end;
    }
    if (pf->stat_path(pf->ctx, rd, 1, &st) || st.kind != CC_PF_DIR) {
        rc = fs_err(out, "not a folder");
        goto end;
    }
    size_t len;
    int ls = pf->list_dir(pf->ctx, rd, FS_LIST_BYTES, &names, &len);
    if (ls == CC_PF_TOO_LARGE) {
        rc = fs_err(out, "the folder holds too many names to list");
        goto end;
    }
    if (ls != CC_PF_OK) {
        rc = fs_err(out, ls == CC_PF_NO_MEMORY ? "out of memory" : "the folder cannot be read");
        goto end;
    }
    size_t cnt = 0, cap = 0, hidden = 0, skipped = 0;
    for (size_t i = 0; i < len; i += strlen(names + i) + 1) cap++;
    list = malloc((cap ? cap : 1) * sizeof *list);
    if (!list) {
        rc = fs_err(out, "out of memory");
        goto end;
    }
    for (size_t i = 0; i < len; i += strlen(names + i) + 1) {
        const char *nm = names + i + 1; /* after the "d"/"f" mark */
        size_t k = strlen(nm);
        if (nm[0] == '.') hidden++;
        else if (jr_utf8_check(nm, k) != -1) skipped++;
        else list[cnt++] = (Name){nm, k};
    }
    qsort(list, cnt, sizeof *list, name_cmp);
    size_t rl = strlen(rd);
    char full[FS_MAX_PATH + 300], tr[FS_MAX_PATH + 1];
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "dir"), jw_str(&w, rd);
    jw_key(&w, "entries"), jw_arr_begin(&w);
    size_t shown = 0;
    int truncated = 0;
    for (size_t i = 0; i < cnt; i++) {
        if (rl + 1 + list[i].n >= sizeof full) {
            skipped++;
            continue;
        }
        memcpy(full, rd, rl);
        size_t at = rl;
        if (!rl || rd[rl - 1] != '/') full[at++] = '/';
        memcpy(full + at, list[i].name, list[i].n + 1);
        cc_pf_stat es, ts = {0, 0, 0};
        if (pf->stat_path(pf->ctx, full, 0, &es)) {
            skipped++; /* gone since the listing */
            continue;
        }
        int target = 0, outside = 0;
        if (es.kind == CC_PF_LINK) {
            /* never followed silently: where it leads, only when that is inside the roots */
            if (!pf->real_path(pf->ctx, full, tr, sizeof tr) && in_roots(tr) && !pf->stat_path(pf->ctx, tr, 1, &ts))
                target = ts.kind;
            else outside = 1;
        }
        int filelike = es.kind == CC_PF_FILE || (es.kind == CC_PF_LINK && target == CC_PF_FILE);
        if (filelike && !ext_ok(list[i].name, list[i].n, ext)) continue;
        if (!filelike && es.kind != CC_PF_DIR && !(es.kind == CC_PF_LINK && (target == CC_PF_DIR || outside))) continue;
        if (shown == FS_MAX_ENTRIES) {
            truncated = 1;
            break;
        }
        shown++;
        jw_obj_begin(&w);
        jw_key(&w, "kind"), jw_str(&w, kind_text(es.kind));
        jw_key(&w, "mtime"), jw_i64(&w, es.mtime_s);
        jw_key(&w, "name"), jw_str_n(&w, list[i].name, list[i].n);
        if (outside) jw_key(&w, "outside"), jw_bool(&w, 1);
        jw_key(&w, "size"), jw_u64(&w, es.kind == CC_PF_LINK && target == CC_PF_FILE ? ts.size : es.size);
        if (es.kind == CC_PF_LINK) {
            jw_key(&w, "target");
            if (target) jw_str(&w, kind_text(target));
            else jw_null(&w);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "hidden"), jw_u64(&w, hidden);
    /* the folder above, when the rule lets it be listed */
    jw_key(&w, "parent");
    {
        char par[FS_MAX_PATH + 1];
        strcpy(par, rd);
        char *sl = strrchr(par, '/');
        int ok = sl != NULL;
        if (ok) {
            if (sl == par) sl[1] = 0;                        /* "/x" -> "/" */
            else if (sl == par + 2 && par[1] == ':') sl[1] = 0; /* "C:/x" -> "C:/" */
            else *sl = 0;
            ok = strcmp(par, rd) && in_roots(par);
        }
        if (ok) jw_str(&w, par);
        else jw_null(&w);
    }
    jw_key(&w, "skipped"), jw_u64(&w, skipped);
    jw_key(&w, "truncated"), jw_bool(&w, truncated);
    jw_obj_end(&w);
    rc = jw_finish(&w) == JW_OK && !out->oom ? CC_OK : fs_err((buf_clear(out), out), "out of memory");
    if (rc == CC_OK) remember(jr_get(v, "purpose"), rd);
end:
    free(list);
    free(names);
    arena_free(&a);
    return rc;
}

int cc_host_fs_query(const char *name, const char *body, size_t n, Buf *out) {
    if (!strcmp(name, "fs_list")) return fs_list(body, n, out);
    if (!strcmp(name, "fs_home")) { /* {purpose?}: also the folder that purpose listed last */
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = jr_parse(&a, body, n, &je);
        int i = v && v->type == JR_OBJ ? purpose_slot(jr_get(v, "purpose"), 0) : -1;
        JsonW w;
        jw_init(&w, out, 0);
        jw_obj_begin(&w);
        jw_key(&w, "last");
        if (i >= 0) jw_str(&w, last[i].dir);
        else jw_null(&w);
        jw_key(&w, "path");
        if (home_root >= 0) jw_str(&w, roots[home_root]);
        else jw_null(&w);
        jw_obj_end(&w);
        jw_finish(&w);
        arena_free(&a);
        return CC_OK;
    }
    if (!strcmp(name, "fs_roots")) {
        JsonW w;
        jw_init(&w, out, 0);
        jw_arr_begin(&w);
        for (int i = 0; i < nroots; i++) {
            jw_obj_begin(&w);
            jw_key(&w, "kind"), jw_str(&w, i == home_root ? "home" : "root");
            jw_key(&w, "path"), jw_str(&w, roots[i]);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_finish(&w);
        return CC_OK;
    }
    return -1;
}

/* ---- POST /stage: a dropped file (fs.h) ---- */

static char stage_dir[FS_MAX_PATH + 1];
#define FS_STAGED_MAX 256
static char *staged[FS_STAGED_MAX]; /* the files written there, removed at exit */
static int nstaged;

static void rm_path(const char *p, int dir) {
#ifdef _WIN32
    wchar_t *w = cc_win_wide(p);
    if (w) dir ? RemoveDirectoryW(w) : DeleteFileW(w);
    free(w);
#else
    dir ? rmdir(p) : unlink(p);
#endif
}

void cc_host_stage_cleanup(void) {
    for (int i = 0; i < nstaged; i++) rm_path(staged[i], 0), free(staged[i]);
    nstaged = 0;
    if (stage_dir[0]) rm_path(stage_dir, 1);
    stage_dir[0] = 0;
}

static int stage_dir_ready(void) {
    if (stage_dir[0]) return 0;
#ifdef _WIN32
    const cc_platform *pf = cc_platform_native();
    const char *t = getenv("TEMP");
    unsigned r = 0;
    if (pf->random(pf->ctx, &r, sizeof r)) r = (unsigned)(size_t)&r;
    int w = snprintf(stage_dir, sizeof stage_dir, "%s/bimble-ccore-drop-%08x", t && *t ? t : ".", r);
    if (w <= 0 || (size_t)w >= sizeof stage_dir || pf->make_dir(pf->ctx, stage_dir)) return stage_dir[0] = 0, -1;
#else
    const char *t = getenv("TMPDIR");
    int w = snprintf(stage_dir, sizeof stage_dir, "%s/bimble-ccore-drop-XXXXXX", t && *t ? t : "/tmp");
    if (w <= 0 || (size_t)w >= sizeof stage_dir || !mkdtemp(stage_dir)) return stage_dir[0] = 0, -1; /* 0700 */
#endif
    return 0;
}

int cc_host_stage(const char *name, const char *bytes, size_t n, Buf *out) {
    size_t k = strlen(name);
    int ok = k > 0 && k <= 255 && name[0] != '.' && jr_utf8_check(name, k) == -1;
    for (size_t i = 0; ok && i < k; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|", c)) ok = 0; /* Windows' forbidden ones too */
    }
    if (!ok) {
        buf_puts(out, "stage: name: a plain file name (no folder, not hidden, at most 255 bytes)");
        return CC_ERR;
    }
    if (n > FS_STAGE_MAX) {
        buf_puts(out, "stage: the file is too large to drop: use File, Open");
        return CC_ERR;
    }
    const cc_platform *pf = cc_platform_native();
    char path[FS_MAX_PATH + 300];
    if (stage_dir_ready() || (size_t)snprintf(path, sizeof path, "%s/%s", stage_dir, name) >= sizeof path ||
        pf->write_file(pf->ctx, path, bytes, n)) {
        buf_puts(out, "stage: cannot write the dropped file");
        return CC_ERR;
    }
    int seen = 0;
    for (int i = 0; i < nstaged && !seen; i++) seen = !strcmp(staged[i], path);
    if (!seen && nstaged < FS_STAGED_MAX && (staged[nstaged] = malloc(strlen(path) + 1))) strcpy(staged[nstaged++], path);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "path"), jw_str(&w, path);
    jw_obj_end(&w);
    jw_finish(&w);
    return CC_OK;
}
