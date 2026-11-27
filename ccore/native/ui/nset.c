/* The native app's own settings (ticket NUI2): the keys the session's settings file does not know yet (session/settings.c
 * refuses an unknown field), kept in <config dir>/native.json as one JSON object, sorted, each value a JSON literal; a run's
 * --set key=value overrides the file for that run only. A key the session's settings later accept is read there first
 * (optimisations_apply): no second key. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

#define NSET_MAX 48
typedef struct { char key[40], val[64]; int run; } Kv;
static Kv kv[NSET_MAX];
static int nkv;
static char path[1200];

static Kv *find(const char *k) {
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].key, k)) return &kv[i];
    return NULL;
}
static Kv *put(const char *k, const char *v) {
    Kv *e = find(k);
    if (!e && nkv < NSET_MAX) e = &kv[nkv++], snprintf(e->key, sizeof e->key, "%s", k);
    if (e) snprintf(e->val, sizeof e->val, "%s", v);
    return e;
}

void nset_load(App *a) {
    nkv = 0;
    if (!a->config_dir[0]) return;
    snprintf(path, sizeof path, "%s/native.json", a->config_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    Json j = {0};
    if (json_parse(&j, buf, n) == 0 && j.root->type == JR_OBJ)
        for (size_t i = 0; i < j.root->n; i++) {
            const JrMember *m = &j.root->members[i];
            char k[40], v[64];
            snprintf(k, sizeof k, "%.*s", (int)(m->key_n < 39 ? m->key_n : 39), m->key);
            if (m->val.type == JR_BOOL) snprintf(v, sizeof v, "%s", m->val.b ? "true" : "false");
            else if (m->val.type == JR_NUM) snprintf(v, sizeof v, "%.17g", m->val.num);
            else if (m->val.type == JR_STR) snprintf(v, sizeof v, "\"%.*s\"", (int)(m->val.n < 60 ? m->val.n : 60), m->val.s);
            else continue;
            put(k, v);
        }
    json_free(&j);
}

static int cmpkv(const void *x, const void *y) { return strcmp(((const Kv *)x)->key, ((const Kv *)y)->key); }

static void save(void) {
    if (!path[0]) return;
    qsort(kv, (size_t)nkv, sizeof *kv, cmpkv);
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("{", f);
    int c = 0;
    for (int i = 0; i < nkv; i++) {
        if (kv[i].run) continue; /* a run's --set is not written */
        fprintf(f, "%s\"%s\":%s", c++ ? "," : "", kv[i].key, kv[i].val);
    }
    fputs("}\n", f);
    if (fclose(f) == 0) rename(tmp, path);
}

/* the JSON literal of `key` (NULL: not set) */
const char *nset_get(const char *key) {
    Kv *e = find(key);
    return e ? e->val : NULL;
}
double nset_num(const char *key, double dflt) {
    const char *v = nset_get(key);
    if (!v) return dflt;
    if (!strcmp(v, "true")) return 1;
    if (!strcmp(v, "false")) return 0;
    char *end;
    double d = strtod(v[0] == '"' ? v + 1 : v, &end);
    return end != v ? d : dflt;
}
/* set and written (UI); `run` 1: this run only (--set) */
void nset_set(const char *key, const char *json, int run) {
    Kv *e = put(key, json);
    if (!e) return;
    if (run) e->run = 1;
    else e->run = 0, save();
}
