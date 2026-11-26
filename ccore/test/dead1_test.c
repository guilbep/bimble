/* Suite dead1 (ticket DEAD1, ADR 0074 "Nothing fake"): what the core refuses is told, and a
 * default never errors.
 *   - query level_refusals (a C addition, calls.h): on golden/frame.json, Remove of each level
 *     that holds elements and Building storey unticked on it are refused with the verbs' own texts
 *     (level_remove / level_set run dry: no step, no rev change); a new empty level is free.
 * Before DEAD1: level_refusals was an unknown query. */
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"

static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    buf_init(&b);
    *st = cc_call(s, op, name, body, strlen(body), &b);
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

#define FOND "04Aobkc8v6$94cGl_9HNVX"
#define N1 "1uAZA0Yfn05f$xRd1k4uPR"

int main(void) {
    cc_session *s = cc_session_new(cc_platform_native());
    int st;
    char *t = ask(s, "query", "level_refusals", "{}", &st);
    check_true("no model: level_refusals {}", st == CC_OK && !strcmp(t, "{}"));
    free(t);
    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    uint64_t rev0 = s->rev;
    size_t undo0 = s->hist.nundo;
    t = ask(s, "query", "level_refusals", "{}", &st);
    printf("  %s\n", t);
    check_true("level_refusals: answered", st == CC_OK);
    check_true("level_refusals: Remove of Fondations refused with the verb's text",
               has(t, "\"" FOND "\":{\"remove\":\"level_remove: the level Fondations holds 12 elements: move or delete them first\""));
    check_true("level_refusals: Building storey of Fondations refused with the verb's text",
               has(t, "\"storey\":\"level_set: the level Fondations holds 12 elements: it stays a storey\""));
    check_true("level_refusals: the three levels, keys in id order", has(t, "{\"" FOND "\":") && has(t, "\"1jLno3kWj4Hfl6pUGBDd7E\":") && has(t, "\"" N1 "\":"));
    check_true("level_refusals: no step, no rev change", s->rev == rev0 && s->hist.nundo == undo0);
    free(t);
    free(ask(s, "verb", "level_add", "{\"name\":\"Toiture\",\"level_mm\":9000}", &st));
    check_true("a new empty level", st == CC_OK);
    t = ask(s, "query", "level_refusals", "{}", &st);
    check_true("level_refusals: the empty level is free (remove and storey null)", has(t, "{\"remove\":null,\"storey\":null}"));
    free(t);

    cc_session_free(s);
    return report("dead1");
}
