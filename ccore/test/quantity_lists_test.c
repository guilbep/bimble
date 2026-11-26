/* Suite quantity_lists (ticket QTY1, ADR 0080): saved quantity lists on the 72-element frame
 * (golden/frame.json): the verbs schedule_add, schedule_edit, schedule_remove (one history step
 * each, undo to the same bytes), the C-only columns (count, type, gross volume, a property), the
 * filter, the sort, grouping by material then level, a summary, the CSV with given headers, the
 * file round trip (a model without lists keeps its bytes; a list's C keys are written and read
 * back), and hostile lists in a file (refused, no crash). Rust has none of this: the values are
 * hand-computed from the frame's generators (comments), DEVIATIONS.md section 38. On the code
 * before QTY1 the verbs are unknown and the first check fails. */
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/history.h"
#include "../src/model.h"
#include "../src/quantity.h"
#include "../src/scene.h"
#include "../src/verbs.h"
#include "../src/viewfile.h"
#include "check.h"

static int load(cc_model *m) {
    size_t n;
    char *t = slurp("golden", "frame.json", &n);
    cc_error e;
    int rc = !t || cc_model_load(m, t, n, &e);
    if (rc) printf("load frame.json: %s\n", t ? e.msg : "missing");
    free(t);
    return rc;
}

static int run(cc_model *m, cc_history *h, const char *verb, const char *args, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_verb_run(m, verb, args, strlen(args), &l, err);
    if (rc == 0) {
        rc = (int)l.n;
        if (l.n && cc_history_apply(h, m, l.v, l.n, err)) rc = -1;
    }
    cc_cmds_free(&l);
    return rc;
}

/* The CSV of the list `name` (effective), with `headers` when given. */
static void csv_of(const cc_model *m, const char *name, const cc_str *headers, size_t nh, Buf *out) {
    cc_scene sc;
    cc_error e;
    buf_clear(out);
    if (cc_scene_build(&sc, m, &e)) return;
    cc_viewfile_meta mt;
    if (!cc_viewfile_meta_from_model(&mt, m, &sc)) {
        const cc_schedule *v[256];
        size_t n = cc_schedules_effective(m->schedules, m->nschedules, v, 256);
        for (size_t i = 0; i < n; i++)
            if (v[i]->name.n == strlen(name) && !memcmp(v[i]->name.p, name, v[i]->name.n)) {
                cc_sched_table t;
                if (!cc_schedule_table(&t, &sc, &mt, v[i], &e)) cc_schedule_csv_headers(out, &t, headers, nh);
                cc_sched_table_free(&t);
            }
        cc_viewfile_meta_free(&mt);
    }
    cc_scene_free(&sc);
}

static void is(const char *what, const Buf *got, const char *want) {
    check_eq(what, got->p ? got->p : "", got->len, want, strlen(want));
}

static void lists(void) {
    cc_model m;
    if (load(&m)) {
        check_true("frame.json loads", 0);
        return;
    }
    cc_history h;
    cc_history_init(&h);
    Buf err, before, csv, js;
    buf_init(&err), buf_init(&before), buf_init(&csv), buf_init(&js);
    cc_model_save(&m, &before);

    /* New quantities: a list of the columns, one step */
    check_true("schedule_add: one command",
               run(&m, &h, "schedule_add", "{\"name\":\"Poteaux\",\"classes\":[\"IfcColumn\"],\"columns\":[\"level\",\"name\",\"count\",\"volume\"]}",
                   &err) == 1);
    if (h.nundo != 1) {
        printf("  %s\n", buf_cstr(&err));
        goto end;
    }
    check_true("schedule_add: one history step, labelled", !strcmp(h.undo[0].label, "add the schedule Poteaux"));
    check_true("schedule_add: stored", m.nschedules == 1 && m.schedules[0].nfields == 4);
    /* Hand: 12 columns a level, each 300 x 300 x 2780 mm = 0.2502 m3; a level 12 x 0.2502 = 3.0024,
     * both 24 x 0.2502 = 6.0048 m3 ("6.005"; the count 24). */
    csv_of(&m, "Poteaux", NULL, 0, &csv);
    const char *tail = "Niveau 2 (12),,12,3.002\nTotal (24),,24,6.005\n";
    check_true("Poteaux: subtotal and total (24 columns, 6.005 m3)",
               csv.len > strlen(tail) && !memcmp(csv.p + csv.len - strlen(tail), tail, strlen(tail)));
    const char *head = "Level,Name,Count,Volume (m\xc2\xb3)\n";
    check_true("Poteaux: header", csv.len > strlen(head) && !memcmp(csv.p, head, strlen(head)));
    /* refused: a name taken (a built-in one, any case), an unknown column, bad types */
    check_true("schedule_add taken (built-in, case)", run(&m, &h, "schedule_add", "{\"name\":\"all ELEMENTS\"}", &err) < 0);
    is("taken text", &err, "schedule_add: a quantity list is already named all ELEMENTS");
    check_true("schedule_add unknown column", run(&m, &h, "schedule_add", "{\"columns\":[\"colour\"]}", &err) < 0);
    is("unknown column text", &err, "bad arguments: `columns`: unknown field \"colour\"");
    check_true("schedule_add unknown argument", run(&m, &h, "schedule_add", "{\"new_name\":\"x\"}", &err) < 0);
    check_true("schedule_add no column", run(&m, &h, "schedule_add", "{\"name\":\"X\",\"columns\":[]}", &err) < 0);
    is("no column text", &err, "schedule_add: schedule X: 1 to 32 columns");
    check_true("schedule_add default name", run(&m, &h, "schedule_add", "{}", &err) == 1 && m.nschedules == 2 &&
                                                m.schedules[1].name.n == 10 && !memcmp(m.schedules[1].name.p, "Schedule 1", 10));

    /* Columns: chosen and ordered, the C additions; a property column; the filter; the sort */
    check_true("schedule_edit columns", run(&m, &h, "schedule_edit",
                                            "{\"name\":\"poteaux\",\"columns\":[\"name\",\"count\",\"gross_volume\",\"mass\",\"type\"],"
                                            "\"properties\":[\"Pset_ColumnCommon/Reference\"],\"group_by_level\":false,"
                                            "\"filter\":{\"column\":\"level\",\"value\":\"niveau 1\"},\"sort\":{\"column\":\"name\",\"descending\":true}}",
                                            &err) == 1);
    check_true("schedule_edit label", !strcmp(h.undo[h.nundo - 1].label, "change the schedule Poteaux"));
    /* Hand: Niveau 1 only, 12 columns, names P1.32 down to P1.00; gross = net (no opening) 0.2502
     * each, 12 x 0.2502 = 3.0024 ("3.002"); mass 0.2502 x 2500 = 625.5 ("626", rounded half to
     * even of the exact f64 product) and 12 x 625.5 = 7506 ("7506"); no PredefinedType, no property. */
    csv_of(&m, "Poteaux", NULL, 0, &csv);
    is("Poteaux filtered, sorted down, C columns", &csv,
       "Name,Count,Gross volume (m\xc2\xb3),Mass (kg),Type,Pset_ColumnCommon/Reference\n"
       "P1.32,1,0.250,626,,\nP1.31,1,0.250,626,,\nP1.30,1,0.250,626,,\nP1.22,1,0.250,626,,\nP1.21,1,0.250,626,,\n"
       "P1.20,1,0.250,626,,\nP1.12,1,0.250,626,,\nP1.11,1,0.250,626,,\nP1.10,1,0.250,626,,\nP1.02,1,0.250,626,,\n"
       "P1.01,1,0.250,626,,\nP1.00,1,0.250,626,,\nTotal (12),12,3.002,7506,,\n");
    /* the CSV's headers as the page shows them */
    cc_str hd[] = {{"Nom", 3}, {"Nombre", 6}, {"Volume brut (m\xc2\xb3)", 17}, {"Masse (kg)", 10}, {"Type", 4}, {"R\xc3\xa9" "f.", 5}};
    csv_of(&m, "Poteaux", hd, 6, &csv);
    const char *given = "Nom,Nombre,Volume brut (m\xc2\xb3),Masse (kg),Type,R\xc3\xa9" "f.\n";
    check_true("CSV headers given", csv.len > strlen(given) && !memcmp(csv.p, given, strlen(given)));
    csv_of(&m, "Poteaux", hd, 5, &csv);
    check_true("CSV headers of the wrong count: the titles", csv.len > 4 && !memcmp(csv.p, "Name,", 5));
    check_true("schedule_edit no change: no step", run(&m, &h, "schedule_edit", "{\"name\":\"Poteaux\",\"group_by_level\":false}", &err) == 0);
    check_true("schedule_edit rename", run(&m, &h, "schedule_edit", "{\"name\":\"Poteaux\",\"new_name\":\"Columns N1\"}", &err) == 1);
    check_true("schedule_edit rename label", !strcmp(h.undo[h.nundo - 1].label, "rename the schedule Poteaux to Columns N1"));
    check_true("schedule_edit rename a built-in refused", run(&m, &h, "schedule_edit", "{\"name\":\"Slabs\",\"new_name\":\"Dalles\"}", &err) < 0);
    check_true("schedule_edit a built-in: stored changed",
               run(&m, &h, "schedule_edit", "{\"name\":\"Slabs\",\"columns\":[\"name\",\"area\"]}", &err) == 1 && m.nschedules == 3);
    check_true("schedule_edit unknown list", run(&m, &h, "schedule_edit", "{\"name\":\"Nope\"}", &err) < 0);
    check_true("schedule_edit filter null removes it",
               run(&m, &h, "schedule_edit", "{\"name\":\"Columns N1\",\"filter\":null,\"sort\":null}", &err) == 1 &&
                   !m.schedules[0].has_filter && !m.schedules[0].has_sort);

    /* By material: the preset, grouped by material then level, subtotals only.
     * Hand (one material, C30/37 at 2500 kg/m3):
     *   Fondations: 12 pad footings 1200 x 1200 x 800: area 1.44 m2 (plan), volume 1.152 m3 each:
     *     17.28 m2, 13.824 m3, 34560 kg.
     *   Niveau 1 (and 2): 12 columns (area: the section 0.09 m2, 0.2502 m3), 9 beams 250 x 500 x 6000
     *     (area: plan 1.5 m2, 0.75 m3), 8 beams 250 x 500 x 5000 (1.25 m2, 0.625 m3), 1 slab
     *     18300 x 10300 x 220 (188.49 m2, 41.4678 m3): 30 elements, 1.08 + 13.5 + 10 + 188.49 =
     *     213.07 m2, 3.0024 + 6.75 + 5 + 41.4678 = 56.2202 m3 ("56.220"), 140550.5 kg by hand: the
     *     sum of the rows' unrounded products from the f32 meshes falls just under the half, "140550"
     *     (the same cell as the golden all-elements.csv's level subtotal).
     *   C30/37: 72 elements, 17.28 + 2 x 213.07 = 443.42 m2, 13.824 + 2 x 56.2202 = 126.2644 m3. */
    check_true("schedule_add preset by_material", run(&m, &h, "schedule_add", "{\"preset\":\"by_material\"}", &err) == 1);
    csv_of(&m, "By material", NULL, 0, &csv);
    printf("  By material:\n%s", buf_cstr(&csv));
    is("By material CSV", &csv,
       "Material,Level,Count,Area (m\xc2\xb2),Volume (m\xc2\xb3),Mass (kg)\n"
       "C30/37 (72),,72,443.42,126.264,315661\n"
       "Fondations (12),,12,17.28,13.824,34560\n"
       "Niveau 1 (30),,30,213.07,56.220,140550\n"
       "Niveau 2 (30),,30,213.07,56.220,140550\n"
       "Total (72),,72,443.42,126.264,315661\n");

    /* save and reopen: the lists and their C keys come back the same; the bytes again */
    buf_clear(&js);
    cc_model_save(&m, &js);
    {
        cc_model m2;
        cc_error e;
        int bad = cc_model_load(&m2, js.p, js.len, &e);
        check_true("reopen: loads", !bad);
        if (!bad) {
            Buf js2;
            buf_init(&js2);
            cc_model_save(&m2, &js2);
            check_eq("reopen: same bytes", js2.p, js2.len, js.p, js.len);
            check_true("reopen: 4 lists", m2.nschedules == 4 && m2.schedules[3].group_by_material && m2.schedules[3].summary);
            buf_free(&js2);
            cc_model_free(&m2);
        }
    }
    check_true("the file holds the C keys (pretty JSON)", strstr(buf_cstr(&js), "\"group_by_material\": true") &&
                                                             strstr(buf_cstr(&js), "\"summary\": true") &&
                                                             strstr(buf_cstr(&js), "\"Pset_ColumnCommon/Reference\""));
    /* remove: one of the model's own; a changed built-in back to built-in; an unchanged one refused */
    check_true("schedule_remove", run(&m, &h, "schedule_remove", "{\"name\":\"Schedule 1\"}", &err) == 1 && m.nschedules == 3);
    check_true("schedule_remove a changed built-in", run(&m, &h, "schedule_remove", "{\"name\":\"slabs\"}", &err) == 1 && m.nschedules == 2);
    check_true("schedule_remove an unchanged built-in refused", run(&m, &h, "schedule_remove", "{\"name\":\"Walls\"}", &err) < 0);
    /* undo every step: the frame's bytes (a model without lists keeps them) */
    while (cc_history_undo(&h, &m)) {}
    buf_clear(&js);
    cc_model_save(&m, &js);
    check_eq("undo all: frame.json's bytes", js.p, js.len, before.p, before.len);
    check_true("frame.json has no schedules key", !strstr(buf_cstr(&before), "\"schedules\""));
end:
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&err), buf_free(&before), buf_free(&csv), buf_free(&js);
}

/* A file is untrusted input: hostile lists are refused with a message, never a crash. */
static void hostile(void) {
    size_t n;
    char *t = slurp("golden", "frame.json", &n);
    if (!t) return check_true("frame.json", 0);
    const char *key = "\"storeys\":";
    char *at = strstr(t, key);
    if (!at) {
        free(t);
        return check_true("frame.json has storeys", 0);
    }
    static const char *const BAD[] = {
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"colour\"]}]",                                   /* unknown field id */
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\"],\"group_by_material\":1}]",            /* a number */
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\"],\"filter\":{\"column\":\"\",\"value\":\"a\"}}]", /* empty column */
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\"],\"sort\":{\"column\":\"name\",\"up\":true}}]",  /* unknown key */
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\"],\"summary\":true,\"summary\":true}]",   /* twice */
        "[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\","
        "\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\","
        "\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\",\"name\"],\"properties\":[\"a\",\"b\"]}]", /* 33 columns */
    };
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        buf_clear(&b);
        buf_append(&b, t, (size_t)(at - t));
        buf_puts(&b, "\"schedules\":");
        buf_puts(&b, BAD[i]);
        buf_putc(&b, ',');
        buf_append(&b, at, n - (size_t)(at - t));
        cc_model m;
        cc_error e;
        int rc = cc_model_load(&m, b.p, b.len, &e);
        char what[64];
        snprintf(what, sizeof what, "hostile list %zu refused", i);
        check_true(what, rc != 0);
        if (rc == 0) cc_model_free(&m);
        else printf("  %s: %s\n", what, e.msg);
    }
    /* a property key of 201 bytes */
    buf_clear(&b);
    buf_append(&b, t, (size_t)(at - t));
    buf_puts(&b, "\"schedules\":[{\"name\":\"X\",\"classes\":[],\"fields\":[\"name\"],\"properties\":[\"");
    for (int i = 0; i < 201; i++) buf_putc(&b, 'p');
    buf_puts(&b, "\"]}],");
    buf_append(&b, at, n - (size_t)(at - t));
    cc_model m;
    cc_error e;
    int rc = cc_model_load(&m, b.p, b.len, &e);
    check_true("hostile property key of 201 bytes refused", rc != 0);
    if (rc == 0) cc_model_free(&m);
    buf_free(&b);
    free(t);
}

int main(void) {
    lists();
    hostile();
    return report("quantity_lists");
}
