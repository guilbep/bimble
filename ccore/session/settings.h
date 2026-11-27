/* The user's settings (ticket SET1): the eleven fields of the Rust viewer's `Settings`
 * (crates/bimble-viewer/src/settings.rs:7-35), read as its `from_json` reads them, written as its
 * `to_json` writes them, in the SAME file (settings_file.h: the folder the carrier picks, or the
 * session's memory when there is none: the wasm module, whose page keeps the text in the
 * browser's localStorage under Rust's key `bimble.settings`, settings.rs:246).
 *
 *   query settings          {applies, file, fonts, needs, note, text_sizes, themes, values}
 *                           values: the 11 fields, Rust's JSON names and defaults (to_json), and
 *                           the lab's `renderer` (GPU1: auto, webgl2, webgpu);
 *                           applies: whether this build uses each field; needs: what a field that
 *                           does not apply would need; note: why the file did not read (or null)
 *   query settings_text     the file's text as it is stored now, as a JSON string (null: none);
 *                           the wasm page keeps it
 *   action set_settings {field: value, ...}
 *                           validated as a whole first (Rust's ranges and ids; an unknown field,
 *                           `recent` included, refused), then written: every field Rust writes,
 *                           any other member of the file kept as it is. Session state: no history
 *                           step, rev unchanged.
 *   action settings_load {"text": "..."}
 *                           the text the page kept (wasm): the session's settings, in memory
 *   state.user              calls.rs:253: {email, name, trigram} from user_name / user_email,
 *                           null when no name is set
 *
 * L5: files only through cc_platform (settings_file.c). Not in the viewer-only wasm (stubs). */
#ifndef CCORE_SESSION_SETTINGS_H
#define CCORE_SESSION_SETTINGS_H
#include <stddef.h>
#include "buf.h"
#include "json_r.h"
#include "json_w.h"

typedef struct cc_session cc_session;

#define CC_SETTINGS_LINE 120                           /* settings.rs short_line: 120 characters */
#define CC_SETTINGS_LINE_BYTES (CC_SETTINGS_LINE * 4 + 1)

typedef struct {
    double memory_share;  /* 0.2..=1.0, 0.8 */
    unsigned moving_fps;  /* 0..=240, 30 */
    int moving_proxies;   /* true */
    int gpu_cull;         /* false */
    char theme[24];       /* ui_theme.rs THEMES ids, "grey" */
    char ui_font[12];     /* "roboto" | "ubuntu" */
    float text_size;      /* f32, 10..=16, 12 */
    char user_name[CC_SETTINGS_LINE_BYTES];
    char user_email[CC_SETTINGS_LINE_BYTES];
    char coordinates[16]; /* "survey" | "wgs84" | "utm" | "epsg:<n>" */
    /* GPU1 (ADR 0083): the lab's 3D renderer, "auto" | "webgl2" | "webgpu" ("auto"). Not one of Rust's
     * fields (its viewer chose wgpu's backend itself): not in to_json, written to the file only once
     * set (a file without it keeps its bytes); query settings lists it in `values`. */
    char renderer[8];
    /* SIZE3 (ADR 0078 amendment 5): the GPU memory the links' detail may take in the lab's 3D view, MB
     * (64..=65536, 768). Not Rust's (its viewer had no budget across links): as `renderer`, written to the
     * file only once set, listed in query settings' `values`. */
    unsigned link_gpu_mb;
    /* SHELL1 (ADR 0078 amendment 7): the outside walk over the links' baked exterior shell (tiles.h CC_TILES_SEL_SHELL),
     * "off" | "outside" (wherever baked) | "auto" (where the eye is outside a link's box, no section cuts it, none of its
     * exterior elements hidden) ("auto"). Not Rust's: as `renderer`, written once set. */
    char shell_mode[8];
} cc_settings;
#define CC_LINK_GPU_MB_DEFAULT 768u

/* Settings::default(). */
void cc_settings_default(cc_settings *s);
/* Settings::from_json on a parsed value (NULL, or not an object: the defaults). */
void cc_settings_of(const JrVal *v, cc_settings *s);
/* Settings::from_json on a text (a text that does not parse: the defaults, and -1). */
int cc_settings_parse(const char *text, size_t n, cc_settings *s);
/* `{field: value, ...}` checked against Rust's ranges and ids, then applied to `s` (all or
 * nothing). 0, or -1 with the reason in `why`. */
int cc_settings_set(cc_settings *s, const JrVal *body, Buf *why);
/* Settings::to_json (recent: `recent[0..n)`): compact, keys sorted, as serde_json writes it. */
void cc_settings_to_json(const cc_settings *s, char *const *recent, size_t nrecent, Buf *out);
/* ribbon.rs trigram: the first three letters of one word, else the initials of the first three
 * words, upper case; 0 when the name is blank. */
int cc_settings_trigram(const char *name, char *out, size_t cap);

/* ---- the session's settings ---- */
int cc_settings_query(cc_session *s, Buf *out);
int cc_settings_text_query(cc_session *s, Buf *out);
int cc_settings_action(cc_session *s, const char *name, const char *body, size_t n, Buf *out);
/* state.user (calls.rs:253). */
void cc_settings_user_json(cc_session *s, JsonW *w);
void cc_settings_free(cc_session *s);

/* query about {graphics?, commit?} (lib.rs:4081): {build, commit, graphics, platform, text,
 * version}; graphics and commit are the page's (the WebGL renderer, the page's build), text is
 * what "Copy for a bug report" copies (lib.rs:4089). In the viewer-only build too. */
int cc_about_json(const char *version, const char *body, size_t n, Buf *out);
#endif
