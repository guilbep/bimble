/* Views, sheets and schedules (view.h): decoding from the JSON/CBOR DOM as serde derive does,
 * writing as serde does (JSON text through JsonW, CBOR through cbor.h, one writer for both),
 * validation with Rust's texts, deep copies (canonical: -0.0 becomes 0.0). Used by file.c (the
 * model's sections), command.c (add_view, set_view, add_sheet, set_sheet, set_schedules) and
 * model.c (Model::validate). */
#ifndef CCORE_VIEW_IO_H
#define CCORE_VIEW_IO_H
#include "json_r.h"
#include "json_w.h"
#include "model.h"

/* ---- decoding: errors as serde words them, without the "parse: " prefix ---- */
typedef struct {
    Arena *a;   /* where strings and arrays go */
    Buf msg;    /* the error text when failed */
    size_t off; /* its input offset */
    int failed;
} cc_vd;

void cc_vd_init(cc_vd *d, Arena *a);
void cc_vd_free(cc_vd *d);
/* `skip`: a member ignored (an internally tagged enum's tag, "op" in a command), or NULL. */
int cc_vd_view(cc_vd *d, const JrVal *v, const char *skip, cc_view *out);
int cc_vd_sheet(cc_vd *d, const JrVal *v, const char *skip, cc_sheet *out);
int cc_vd_schedules(cc_vd *d, const JrVal *v, cc_schedule **out, size_t *n);

/* ---- writing: JSON when `j` is set, else CBOR into `c` ---- */
typedef struct {
    JsonW *j;
    Buf *c;
} cc_em;

/* `op` (JSON only): written first as "op": op, the command's tag. */
void cc_em_view(cc_em *e, const cc_view *v, const char *op);
void cc_em_sheet(cc_em *e, const cc_sheet *s, const char *op);
void cc_em_schedules(cc_em *e, const cc_schedule *s, size_t n);

/* ---- lookups ---- */
const cc_view *cc_model_view(const cc_model *m, const cc_gid *id);
const cc_sheet *cc_model_sheet(const cc_model *m, const cc_gid *id);
/* The default views' ids (view.rs default_views): GlobalId::derived("bimble view 3d"),
 * derived("bimble view plan down|up <storey id>"). */
int cc_view_is_default(const cc_model *m, const cc_gid *id);
/* A view of the model: stored, or a default one (Model::view_ids). */
int cc_view_exists(const cc_model *m, const cc_gid *id);

/* ---- validation: 0 ok, -1 with Rust's text appended to `why` ---- */
int cc_view_validate(const cc_model *m, const cc_view *v, Buf *why);   /* View::validate(levels) */
/* Sheet::validate(view_exists): `m` NULL accepts every view. */
int cc_sheet_validate(const cc_model *m, const cc_sheet *s, Buf *why);
int cc_schedules_validate(const cc_schedule *s, size_t n, Buf *why); /* schedule::validate_all */
int cc_schedule_eq(const cc_schedule *a, const cc_schedule *b);

/* ---- copies into `a`, every f64 made canonical; 0 ok, -1 out of memory ---- */
int cc_view_dup(Arena *a, cc_view *v);
int cc_sheet_dup(Arena *a, cc_sheet *s);
int cc_schedules_dup(Arena *a, cc_schedule **s, size_t n);
/* In place (a loaded model): -0.0 becomes 0.0. */
void cc_view_canonical(cc_view *v);
void cc_sheet_canonical(cc_sheet *s);
#endif
