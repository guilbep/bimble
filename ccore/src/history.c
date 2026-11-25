/* See history.h. */
#include "history.h"
#include <stdlib.h>
#include <string.h>
#include "json_w.h"

void cc_history_init(cc_history *h) { memset(h, 0, sizeof *h); }

static void step_free(cc_step *s) {
    free(s->label);
    free(s->cmds);
}

static void clear(cc_step *v, size_t *n) {
    for (size_t i = 0; i < *n; i++) step_free(&v[i]);
    *n = 0;
}

void cc_history_free(cc_history *h) {
    clear(h->undo, &h->nundo);
    clear(h->redo, &h->nredo);
    free(h->undo);
    free(h->redo);
    memset(h, 0, sizeof *h);
}

static int push(cc_step **v, size_t *n, size_t *cap, cc_step s) {
    if (*n == *cap) {
        size_t c = *cap ? 2 * *cap : 16;
        cc_step *p = realloc(*v, c * sizeof *p);
        if (!p) return -1;
        *v = p;
        *cap = c;
    }
    (*v)[(*n)++] = s;
    return 0;
}

static void reverse(cc_command *c, size_t n) {
    for (size_t i = 0; i + 1 < n - i; i++) {
        cc_command t = c[i];
        c[i] = c[n - 1 - i];
        c[n - 1 - i] = t;
    }
}

static char *label_of(Buf *b) {
    size_t n = b->oom ? 0 : b->len;
    char *s = malloc(n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, b->p, n);
    s[n] = 0;
    return s;
}

int cc_history_apply(cc_history *h, cc_model *m, const cc_command *cmds, size_t n, Buf *err) {
    Buf lb;
    buf_init(&lb);
    cc_command_describe_group(cmds, n, m, &lb);
    cc_step s = {label_of(&lb), n ? malloc(n * sizeof(cc_command)) : NULL, n};
    buf_free(&lb);
    if (!s.label || (n && !s.cmds)) {
        step_free(&s);
        if (err) buf_clear(err), buf_puts(err, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (cc_command_apply(m, &cmds[i], &s.cmds[i], err)) {
            /* Roll back: a just-applied command's inverse cannot fail. */
            for (size_t j = i; j-- > 0;) {
                cc_command again;
                cc_command_apply_inverse(m, &s.cmds[j], &again, NULL);
            }
            step_free(&s);
            return -1;
        }
    }
    reverse(s.cmds, n);
    if (push(&h->undo, &h->nundo, &h->capundo, s)) {
        /* Out of memory: take it back so the model matches the history. */
        for (size_t j = 0; j < n; j++) {
            cc_command again;
            cc_command_apply_inverse(m, &s.cmds[j], &again, NULL);
        }
        step_free(&s);
        if (err) buf_clear(err), buf_puts(err, "out of memory");
        return -1;
    }
    clear(h->redo, &h->nredo);
    return 0;
}

/* Pops a step from `from`, applies its commands, pushes the reversed inverses on `to`. */
static int swap(cc_model *m, cc_step *from, size_t *nfrom, cc_step **to, size_t *nto, size_t *capto) {
    if (!*nfrom) return 0;
    cc_step s = from[--*nfrom];
    for (size_t i = 0; i < s.n; i++) {
        cc_command inv;
        /* The history's own inverses always apply ("history inverse must apply" in Rust). */
        if (cc_command_apply_inverse(m, &s.cmds[i], &inv, NULL) == 0) s.cmds[i] = inv;
    }
    reverse(s.cmds, s.n);
    if (push(to, nto, capto, s)) step_free(&s); /* out of memory: the step is lost */
    return 1;
}

int cc_history_undo(cc_history *h, cc_model *m) {
    return swap(m, h->undo, &h->nundo, &h->redo, &h->nredo, &h->capredo);
}

int cc_history_redo(cc_history *h, cc_model *m) {
    return swap(m, h->redo, &h->nredo, &h->undo, &h->nundo, &h->capundo);
}

static void labels(const cc_step *v, size_t n, size_t max, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t k = 0; k < n && (!max || k < max); k++) jw_str(&w, v[n - 1 - k].label);
    jw_arr_end(&w);
    jw_finish(&w);
}

void cc_history_labels(const cc_history *h, size_t max, Buf *json_undo, Buf *json_redo) {
    if (json_undo) labels(h->undo, h->nundo, max, json_undo);
    if (json_redo) labels(h->redo, h->nredo, max, json_redo);
}

void cc_history_json(const cc_history *h, Buf *out) {
    /* serde_json::json! sorts its keys: "redo" first. */
    buf_puts(out, "{\"redo\":");
    labels(h->redo, h->nredo, 50, out);
    buf_puts(out, ",\"undo\":");
    labels(h->undo, h->nundo, 50, out);
    buf_putc(out, '}');
}
