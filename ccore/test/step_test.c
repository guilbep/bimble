/* Suite step: the STEP parser (src/step.h) and the parallel jobs (src/jobs.h).
 * - parse_skipping (step.rs skipped_types_are_not_stored_and_the_rest_parses_the_same): a
 *   skipped type is neither stored nor counted as malformed; the rest parses the same.
 * - chunks: a synthetic text of many 1 MiB chunks (strings with ';', quotes and newlines,
 *   comments, complex instances, malformed instances, duplicate ids across chunks, ids out of
 *   order) parses to the same entities, byte for byte, on 1 thread and on 8, and as expected.
 * - cc_jobs_run: every task once, worker indices in range, whatever the thread count. */
#include <stdint.h>
#include <string.h>
#include "../src/jobs.h"
#include "../src/step.h"
#include "check.h"

static void dump_value(Buf *b, const cc_sv *v) {
    switch (v->kind) {
    case CC_SV_NULL: buf_putc(b, '$'); break;
    case CC_SV_DERIVED: buf_putc(b, '*'); break;
    case CC_SV_INT: buf_i64(b, v->u.i); break;
    case CC_SV_REAL: {
        uint64_t bits;
        memcpy(&bits, &v->u.r, 8);
        buf_puts(b, "r");
        buf_u64(b, bits);
        break;
    }
    case CC_SV_STR: buf_putc(b, '\''), buf_append(b, v->u.s.p, v->u.s.n), buf_putc(b, '\''); break;
    case CC_SV_ENUM: buf_putc(b, '.'), buf_puts(b, v->u.s.p), buf_putc(b, '.'); break;
    case CC_SV_REF: buf_putc(b, '#'), buf_u64(b, v->u.ref); break;
    case CC_SV_LIST:
        buf_putc(b, '(');
        for (size_t i = 0; i < v->u.list.n; i++) {
            if (i) buf_putc(b, ',');
            dump_value(b, &v->u.list.items[i]);
        }
        buf_putc(b, ')');
        break;
    case CC_SV_TYPED: buf_puts(b, v->u.typed.name), buf_putc(b, '['), dump_value(b, v->u.typed.inner), buf_putc(b, ']'); break;
    }
}

static void dump_file(Buf *b, const cc_step_file *f) {
    buf_puts(b, "skipped ");
    buf_u64(b, f->skipped);
    buf_puts(b, "\n");
    for (size_t k = 0; k < f->nschemas; k++) buf_puts(b, f->schemas[k]), buf_putc(b, '\n');
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *e = &f->ents[i];
        buf_putc(b, '#');
        buf_u64(b, e->id);
        buf_putc(b, '=');
        buf_puts(b, e->name);
        buf_putc(b, '(');
        for (size_t k = 0; k < e->nargs; k++) {
            if (k) buf_putc(b, ',');
            dump_value(b, &e->args[k]);
        }
        buf_puts(b, ")\n");
    }
}

static const char SAMPLE[] = "ISO-10303-21;\n"
                             "HEADER;\n"
                             "FILE_DESCRIPTION(('ViewDefinition [x]'),'2;1');\n"
                             "FILE_SCHEMA(('IFC4'));\n"
                             "ENDSEC;\n"
                             "DATA;\n"
                             "#1=IFCCARTESIANPOINT((0.,1.5,-2.E-3));\n"
                             "#2=IFCWALL('2O2Fr$t4X7Zf8NOew3FLOH',$,'Mur l''est \\X2\\00E9\\X0\\',$,$,#3,$,$,.SOLIDWALL.);\n"
                             "/* comment */\n"
                             "#3=IFCPROPERTYSINGLEVALUE('Fire',$,IFCLABEL('EI60'),$);\n"
                             "#4=IFCBROKEN(#1,,);\n"
                             "#5=(IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)OTHER());\n"
                             "#6=IFCPROPERTYSINGLEVALUE(#1,,);\n"
                             "ENDSEC;\n"
                             "END-ISO-10303-21;\n";

static void skipping(void) {
    static const char *const SKIP[] = {"IFCPROPERTYSINGLEVALUE"};
    cc_step_file all, some;
    cc_error err;
    int r1 = cc_step_parse(&all, SAMPLE, strlen(SAMPLE), &err);
    int r2 = cc_step_parse_skipping(&some, SAMPLE, strlen(SAMPLE), SKIP, 1, &err);
    check_true("sample parses", !r1 && !r2);
    /* #4 and #6 are malformed when parsed; #6 is a skipped type: stepped over, not counted. */
    check_true("all: 2 malformed (#4, #6)", all.skipped == 2);
    check_true("skipping: 1 malformed (#4)", some.skipped == 1);
    check_true("skipping: #3 absent, present in all", !cc_step_get(&some, 3) && cc_step_get(&all, 3));
    check_true("skipping: one fewer entity", some.n + 1 == all.n);
    Buf a, b;
    buf_init(&a), buf_init(&b);
    for (size_t i = 0; i < all.n; i++)
        if (all.ents[i].id != 3) {
            cc_step_file one = all;
            one.ents = &all.ents[i], one.n = 1, one.skipped = 0, one.nschemas = 0;
            dump_file(&a, &one);
        }
    for (size_t i = 0; i < some.n; i++) {
        cc_step_file one = some;
        one.ents = &some.ents[i], one.n = 1, one.skipped = 0, one.nschemas = 0;
        dump_file(&b, &one);
    }
    check_eq("skipping: the rest parses the same", a.p, a.len, b.p, b.len);
    buf_free(&a), buf_free(&b);
    cc_step_free(&all);
    cc_step_free(&some);
}

/* A text of `stmts` statements, several 1 MiB chunks. */
static void synthetic(Buf *t, size_t stmts) {
    buf_puts(t, "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\nFILE_SCHEMA(('IFC4X3','IFC2X3'));\nENDSEC;\nDATA;\n");
    for (size_t i = 1; i <= stmts; i++) {
        uint64_t id = i;
        if (i % 1000 == 8) id = i + 10; /* out of order, and a duplicate: statement i + 10 wins */
        if (i % 99991 == 0) id = 5;     /* a duplicate across chunks: the latest wins */
        buf_putc(t, '#');
        buf_u64(t, id);
        switch (i % 9) {
        case 0:
            buf_puts(t, "=IFCLABELLED('semi;colon ''quote'' ");
            buf_u64(t, i);
            buf_puts(t, "\nnew line;#999=IFCFAKE();',$,.T.);\n");
            break;
        case 1: buf_puts(t, "=IFCCARTESIANPOINT((1.5,-2.E-3,"), buf_u64(t, i), buf_puts(t, ".));\n"); break;
        case 2: buf_puts(t, "= IFCDIRECTION ((0.,0.,1.)) ; /* a comment; with semicolons */\n"); break;
        case 3: buf_puts(t, "=IFCBROKEN(#1,,);\n"); break; /* malformed: skipped and counted */
        case 4: buf_puts(t, "=(IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)OTHER(#2));\n"); break;
        case 5: buf_puts(t, "=IFCPROPERTYSINGLEVALUE('Fire;x',$,IFCLABEL('EI60'),$);\n"); break;
        case 6: buf_puts(t, "=ifcwall('id',$,IFCLENGTHMEASURE(2.5),(#1,#2,(3,4)),\"0A\",*);\n"); break;
        case 7: buf_puts(t, "=IFCPOLYLINE((#1,#2,#3,#4,#5,#6,#7,#8,#9,#10,#11,#12,#13,#14));\n"); break;
        default: buf_puts(t, "=IFCTEXT('\\X2\\00E9\\X0\\ and \\S\\a');\n"); break;
        }
    }
    buf_puts(t, "ENDSEC;\nEND-ISO-10303-21;\n");
}

static void chunks(void) {
    static const char *const SKIP[] = {"IFCPROPERTYSINGLEVALUE", "IFCQUANTITYLENGTH"};
    Buf t, d1, d8, s1, s8;
    buf_init(&t), buf_init(&d1), buf_init(&d8), buf_init(&s1), buf_init(&s8);
    synthetic(&t, 120000);
    check_true("synthetic text: over 4 chunks", t.len > 4 * CC_STEP_CHUNK_BYTES);
    cc_step_file f;
    cc_error err;
    for (int run = 0; run < 2; run++) {
        cc_jobs_set_threads(run ? 8 : 1);
        Buf *d = run ? &d8 : &d1, *s = run ? &s8 : &s1;
        check_true("synthetic parses", !cc_step_parse(&f, t.p, t.len, &err));
        dump_file(d, &f);
        if (!run) {
            check_true("13334 malformed (every 9th from the 3rd)", f.skipped == 13334);
            const cc_step_entity *e5 = cc_step_get(&f, 5);
            check_true("#5: the latest duplicate wins (statement 99991)", e5 && !strcmp(e5->name, "IFCCARTESIANPOINT"));
            const cc_step_entity *e18 = cc_step_get(&f, 18);
            check_true("#18: the later of two (statement 18, not 8)", e18 && !strcmp(e18->name, "IFCLABELLED"));
            check_true("#8: none (statement 8 is #18)", !cc_step_get(&f, 8));
            const cc_step_entity *e4 = cc_step_get(&f, 4);
            check_true("#4: complex instance, first part", e4 && !strcmp(e4->name, "IFCSIUNIT") && e4->nargs == 4);
            int sorted = 1;
            for (size_t i = 1; i < f.n; i++) sorted &= f.ents[i - 1].id < f.ents[i].id;
            check_true("sorted by id, unique", sorted);
        }
        cc_step_free(&f);
        check_true("synthetic parses (skipping)", !cc_step_parse_skipping(&f, t.p, t.len, SKIP, 2, &err));
        dump_file(s, &f);
        if (!run) {
            int none = 1;
            for (size_t i = 0; i < f.n; i++) none &= strcmp(f.ents[i].name, "IFCPROPERTYSINGLEVALUE") != 0;
            check_true("skipping: no IFCPROPERTYSINGLEVALUE", none);
        }
        cc_step_free(&f);
    }
    cc_jobs_set_threads(0);
    check_eq("1 thread and 8: the same entities", d1.p, d1.len, d8.p, d8.len);
    check_eq("1 thread and 8: the same entities (skipping)", s1.p, s1.len, s8.p, s8.len);
    check_true("skipping stores fewer", s1.len < d1.len);
    buf_free(&t), buf_free(&d1), buf_free(&d8), buf_free(&s1), buf_free(&s8);
}

typedef struct {
    size_t n, workers;
    unsigned char *hit;
    size_t *worker;
} JobsCheck;

static void count_job(void *ctx, size_t task, size_t w) {
    JobsCheck *j = ctx;
    j->hit[task]++;
    j->worker[task] = w;
}

static void jobs(void) {
    static const size_t THREADS[] = {1, 3, 8, CC_JOBS_MAX};
    for (size_t k = 0; k < sizeof THREADS / sizeof *THREADS; k++) {
        cc_jobs_set_threads(THREADS[k]);
        JobsCheck j = {10000, cc_jobs_workers(10000), calloc(10000, 1), calloc(10000, sizeof(size_t))};
        cc_jobs_run(j.n, count_job, &j);
        int once = 1, range = 1;
        for (size_t i = 0; i < j.n; i++) once &= j.hit[i] == 1, range &= j.worker[i] < j.workers;
        check_true("cc_jobs_run: every task once", once);
        check_true("cc_jobs_run: worker in range", range);
        check_true("cc_jobs_workers: at most the threads, at most the tasks",
                   j.workers >= 1 && j.workers <= cc_jobs_threads() && cc_jobs_workers(2) <= 2 && cc_jobs_workers(0) == 1);
        free(j.hit), free(j.worker);
    }
    cc_jobs_set_threads(0);
    check_true("cc_jobs_threads: at least 1", cc_jobs_threads() >= 1);
    cc_jobs_run(0, count_job, NULL); /* no tasks: nothing called */
}

/* FIXLIB: the nesting bound (step.rs MAX_DEPTH 64: a value deeper than 64 is refused). The
 * arguments' list is depth 1, so 63 lists inside it put the innermost value at depth 64 (read)
 * and 64 lists at 65 (the statement refused, the next one read). */
static void depth_bound(void) {
    for (int k = 63; k <= 64; k++) {
        Buf t;
        buf_init(&t);
        buf_puts(&t, "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=IFCX(");
        for (int i = 0; i < k; i++) buf_putc(&t, '(');
        buf_putc(&t, '1');
        for (int i = 0; i < k; i++) buf_putc(&t, ')');
        buf_puts(&t, ");\n#2=IFCY(2);\nENDSEC;\nEND-ISO-10303-21;\n");
        cc_step_file f;
        cc_error err;
        int rc = cc_step_parse(&f, t.p, t.len, &err);
        printf("  depth %d lists: rc %d, %zu entities, %zu skipped\n", k, rc, rc ? (size_t)0 : f.n, rc ? (size_t)0 : f.skipped);
        if (k == 63) check_true("63 nested lists (depth 64): read", rc == 0 && f.n == 2 && f.skipped == 0);
        else check_true("64 nested lists (depth 65): the statement refused, the next read", rc == 0 && f.n == 1 && f.skipped == 1);
        if (rc == 0) cc_step_free(&f);
        buf_free(&t);
    }
}

int main(void) {
    depth_bound();
    skipping();
    chunks();
    jobs();
    return report("step");
}
