/* Suite settings (ticket SET1, session/settings.h): the user's settings as the Rust viewer reads
 * and writes them (crates/bimble-viewer/src/settings.rs), in the same file.
 *   - the defaults are Rust's (golden/parity/settings.json, read from settings.rs by PAR2) and
 *     Settings::default().to_json() (golden/settings/default.json);
 *   - a file in Rust's form is read and written back byte for byte (golden/settings/);
 *   - every field is set and read back; a bad value is refused, the file untouched;
 *   - members Rust does not know are kept; a broken file gives the defaults and a note;
 *   - the session without a folder (the wasm module) keeps the text in memory (settings_load,
 *     settings_text); state.user (calls.rs:253) and query about.
 * Never the user's real folder: every file is under out/test-settings (BIMBLE_CONFIG_DIR is set to
 * it too, for anything that would look). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define setenv(k, v, o) _putenv_s(k, v)
#else
int setenv(const char *name, const char *value, int overwrite); /* POSIX */
#endif
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/settings.h"
#include "../session/settings_file.h"
#include "../src/arena.h"
#include "../src/json_r.h"
#include "check.h"

#define DIR "out/test-settings"
#define FILE_ DIR "/conf/settings.json"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The answer as a heap string. */
static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    *st = call(s, op, name, body, &b);
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

static char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)l + 1);
    *n = fread(p, 1, (size_t)l, f);
    p[*n] = 0;
    fclose(f);
    return p;
}

static void write_all(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void check_file(const char *what, const char *want) {
    size_t n = 0;
    char *got = read_all(FILE_, &n);
    check_eq(what, got ? got : "(none)", got ? n : 6, want, strlen(want));
    free(got);
}

/* A session on the settings file FILE_ (as the host sets it up: host/main.c). */
static cc_session *session_on_file(void) {
    cc_session *s = cc_session_new(cc_platform_native());
    cc_recent_setup(s, FILE_);
    return s;
}

/* `"key":` and its value text (up to the next ',' or '}' at that level) in `json`, into out. */
static void value_of(const char *json, const char *key, char *out, size_t cap) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    out[0] = 0;
    if (!p) return;
    p += strlen(k);
    int depth = 0, str = 0;
    size_t o = 0;
    for (; *p && o + 1 < cap; p++) {
        if (str) {
            if (*p == '\\' && p[1]) out[o++] = *p++;
            else if (*p == '"') str = 0;
        } else if (*p == '"') str = 1;
        else if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') {
            if (!depth) break;
            depth--;
        } else if (*p == ',' && !depth) break;
        out[o++] = *p;
    }
    out[o] = 0;
}

/* The "values" object of query settings. */
static char *values(cc_session *s) {
    int st;
    char *q = ask(s, "query", "settings", "", &st);
    char *v = strstr(q, "\"values\":");
    const char *from = v ? v + 9 : "";
    char *r = malloc(strlen(from) + 1);
    strcpy(r, from);
    if (r[0]) r[strlen(r) - 1] = 0; /* the closing brace of the answer */
    free(q);
    return r;
}

static void reset_dir(void) {
    remove(FILE_);
    remove(DIR "/conf/settings.json.tmp");
    const cc_platform *pf = cc_platform_native();
    pf->make_dir(pf->ctx, "out");
    pf->make_dir(pf->ctx, DIR);
    pf->make_dir(pf->ctx, DIR "/conf");
}

int main(void) {
    setenv("BIMBLE_CONFIG_DIR", DIR "/conf", 1);
    reset_dir();
    int st;

    /* ---- the defaults: Rust's (PAR2's settings.json, read from settings.rs) ---- */
    {
        size_t n;
        char *par = slurp("golden/parity", "settings.json", &n);
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = jr_parse(&a, par, n, &je);
        const JrVal *fields = v ? jr_get(v, "fields") : NULL;
        cc_session *s = cc_session_new(cc_platform_native());
        char *vals = values(s);
        int nf = 0;
        for (size_t i = 0; fields && i < fields->n; i++) {
            const JrVal *f = &fields->items[i], *name = jr_get(f, "field"), *def = jr_get(f, "default");
            char got[256], what[96];
            value_of(vals, name->s, got, sizeof got);
            snprintf(what, sizeof what, "default %s is Rust's", name->s);
            check_eq(what, got, strlen(got), def->s, def->n);
            nf++;
        }
        check_true("Rust has 11 fields, all checked", nf == 11);
        const JrVal *themes = v ? jr_get(v, "themes") : NULL;
        int st2;
        char *q = ask(s, "query", "settings", "", &st2);
        char th[256];
        value_of(q, "themes", th, sizeof th);
        check_eq("the six themes, Rust's order", th, strlen(th), "[\"grey\",\"dark\",\"light\",\"solarized-dark\",\"solarized-light\",\"high-contrast\"]",
                 strlen("[\"grey\",\"dark\",\"light\",\"solarized-dark\",\"solarized-light\",\"high-contrast\"]"));
        check_true("parity golden lists six themes", themes && themes->n == 6);
        /* Settings::default().to_json() */
        cc_settings d;
        cc_settings_default(&d);
        Buf b;
        buf_init(&b);
        cc_settings_to_json(&d, NULL, 0, &b);
        size_t gn;
        char *g = slurp("golden/settings", "default.json", &gn);
        check_eq("Settings::default().to_json()", b.p, b.len, g, gn);
        free(g), buf_free(&b), free(q), free(vals), free(par);
        arena_free(&a);
        cc_session_free(s);
    }

    /* ---- Rust's files read and written back byte for byte ---- */
    for (const char *const *f = (const char *const[]){"default.json", "filled.json", "unknown.json", NULL}; *f; f++) {
        size_t gn;
        char *g = slurp("golden/settings", *f, &gn);
        reset_dir();
        write_all(FILE_, g);
        cc_session *s = session_on_file();
        char *r = ask(s, "action", "set_settings", "{}", &st);
        char what[128];
        snprintf(what, sizeof what, "%s: set_settings {} -> ok", *f);
        check_true(what, st == CC_OK);
        snprintf(what, sizeof what, "%s written back byte for byte", *f);
        check_file(what, g);
        /* and the parse alone gives Rust's to_json (no unknown member in Rust's own files) */
        if (strcmp(*f, "unknown.json")) {
            cc_settings v;
            check_true("parses", cc_settings_parse(g, gn, &v) == 0);
            char *paths[CC_RECENT_MAX];
            size_t np = 0;
            cc_settings_recent_read(cc_platform_native(), FILE_, paths, &np);
            Buf b;
            buf_init(&b);
            cc_settings_to_json(&v, paths, np, &b);
            snprintf(what, sizeof what, "%s: from_json then to_json", *f);
            check_eq(what, b.p, b.len, g, gn);
            buf_free(&b);
            for (size_t i = 0; i < np; i++) free(paths[i]);
        }
        free(r), free(g);
        cc_session_free(s);
    }
    /* filled.json read: Rust's values */
    {
        reset_dir();
        size_t gn;
        char *g = slurp("golden/settings", "filled.json", &gn);
        write_all(FILE_, g);
        cc_session *s = session_on_file();
        char *v = values(s), got[128];
        value_of(v, "text_size", got, sizeof got);
        check_true("filled: text_size 14.5", !strcmp(got, "14.5"));
        value_of(v, "coordinates", got, sizeof got);
        check_true("filled: coordinates epsg:2263", !strcmp(got, "\"epsg:2263\""));
        value_of(v, "recent", got, sizeof got);
        check_true("filled: the recent files", !strcmp(got, "[\"/a/x.ifc\",\"/b/y.bimble\"]"));
        char *se = ask(s, "query", "state", "", &st);
        check_true("filled: state.user (Ana, ANA)", strstr(se, "\"user\":{\"email\":\"ana@office.example\",\"name\":\"Ana\",\"trigram\":\"ANA\"}") != NULL);
        free(se), free(v), free(g);
        cc_session_free(s);
    }

    /* ---- every field set and read back, into the file ---- */
    {
        reset_dir();
        cc_session *s = session_on_file();
        char *se = ask(s, "query", "state", "", &st);
        check_true("no file: state.user null (no name: Rust's null)", strstr(se, "\"user\":null") != NULL);
        free(se);
        static const char *const SETS[][3] = {
            {"memory_share", "0.35", "0.35"},     {"moving_fps", "120", "120"},          {"moving_fps", "0", "0"},
            {"moving_proxies", "false", "false"}, {"gpu_cull", "true", "true"},          {"theme", "\"dark\"", "\"dark\""},
            {"theme", "\"solarized-dark\"", "\"solarized-dark\""},                       {"ui_font", "\"ubuntu\"", "\"ubuntu\""},
            {"text_size", "13.5", "13.5"},        {"text_size", "12.3", "12.300000190734863"}, /* an f32, as Rust writes it */
            {"text_size", "16", "16.0"},          {"user_name", "\"Ada Lovelace\"", "\"Ada Lovelace\""},
            {"user_email", "\"ada@example.org\"", "\"ada@example.org\""},          {"coordinates", "\"utm\"", "\"utm\""},
            {"coordinates", "\"epsg:21781\"", "\"epsg:21781\""},                    {"coordinates", "\"wgs84\"", "\"wgs84\""},
        };
        for (size_t i = 0; i < sizeof SETS / sizeof SETS[0]; i++) {
            char body[128], what[160], got[128];
            snprintf(body, sizeof body, "{\"%s\":%s}", SETS[i][0], SETS[i][1]);
            char *r = ask(s, "action", "set_settings", body, &st);
            snprintf(what, sizeof what, "set %s -> ok (%s)", body, r);
            check_true(what, st == CC_OK);
            char *v = values(s);
            value_of(v, SETS[i][0], got, sizeof got);
            snprintf(what, sizeof what, "query settings after %s", body);
            check_eq(what, got, strlen(got), SETS[i][2], strlen(SETS[i][2]));
            size_t n;
            char *file = read_all(FILE_, &n);
            snprintf(what, sizeof what, "the file after %s", body);
            char want[160];
            snprintf(want, sizeof want, "\"%s\":%s", SETS[i][0], SETS[i][2]);
            check_true(what, file && strstr(file, want));
            free(file), free(v), free(r);
        }
        /* the file is Rust's form: every field, keys sorted */
        check_file("the file: the eleven fields, sorted, compact",
                   "{\"coordinates\":\"wgs84\",\"gpu_cull\":true,\"memory_share\":0.35,\"moving_fps\":0,\"moving_proxies\":false,\"recent\":[],"
                   "\"text_size\":16.0,\"theme\":\"solarized-dark\",\"ui_font\":\"ubuntu\",\"user_email\":\"ada@example.org\",\"user_name\":\"Ada Lovelace\"}");
        se = ask(s, "query", "state", "", &st);
        check_true("state.user {email, name, trigram} (calls.rs:253)",
                   strstr(se, "\"user\":{\"email\":\"ada@example.org\",\"name\":\"Ada Lovelace\",\"trigram\":\"AL\"}") != NULL);
        free(se);
        /* the change is session state: no history step, the revision unchanged */
        Buf c0, c1;
        buf_init(&c0), buf_init(&c1);
        cc_change_json(s, &c0);
        free(ask(s, "action", "set_settings", "{\"theme\":\"light\"}", &st));
        cc_change_json(s, &c1);
        check_eq("set_settings: the change JSON unchanged (no rev, no history)", c1.p, c1.len, c0.p, c0.len);
        buf_free(&c0), buf_free(&c1);
        /* another session reads it (the next start, or the Rust app) */
        cc_session *s2 = session_on_file();
        char *v2 = values(s2), got[64];
        value_of(v2, "theme", got, sizeof got);
        check_true("a new session reads the file: theme light", !strcmp(got, "\"light\""));
        free(v2);
        cc_session_free(s2);

        /* ---- refused, the file untouched ---- */
        size_t n0;
        char *before = read_all(FILE_, &n0);
        static const char *const BAD[][2] = {
            {"{\"memory_share\":7}", "memory_share"},       {"{\"memory_share\":\"x\"}", "memory_share"},
            {"{\"moving_fps\":241}", "moving_fps"},         {"{\"moving_fps\":1.5}", "moving_fps"},
            {"{\"moving_fps\":-1}", "moving_fps"},          {"{\"moving_proxies\":1}", "moving_proxies"},
            {"{\"gpu_cull\":null}", "gpu_cull"},            {"{\"theme\":\"neon\"}", "theme"},
            {"{\"ui_font\":\"comic\"}", "ui_font"},         {"{\"text_size\":99}", "text_size"},
            {"{\"text_size\":9.5}", "text_size"},           {"{\"text_size\":\"x\"}", "text_size"},
            {"{\"user_name\":\"a\\nb\"}", "user_name"},     {"{\"user_email\":5}", "user_email"},
            {"{\"coordinates\":\"epsg:1\"}", "coordinates"}, {"{\"coordinates\":\"lv95\"}", "coordinates"},
            {"{\"recent\":[]}", "recent"},                  {"{\"bogus\":1}", "unknown field `bogus`"},
            {"[1]", "set_settings"},                        {"{\"theme\":\"dark\",\"text_size\":99}", "text_size"},
        };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            char *r = ask(s, "action", "set_settings", BAD[i][0], &st);
            char what[200];
            snprintf(what, sizeof what, "refused %s: \"%s\" names %s", BAD[i][0], r, BAD[i][1]);
            check_true(what, st != CC_OK && strstr(r, BAD[i][1]) && !strncmp(r, "set_settings", 12));
            free(r);
        }
        {
            char longname[200];
            memset(longname, 'x', 121), longname[121] = 0;
            char body[256];
            snprintf(body, sizeof body, "{\"user_name\":\"%s\"}", longname);
            char *r = ask(s, "action", "set_settings", body, &st);
            check_true("refused: a name of 121 characters", st != CC_OK && strstr(r, "120"));
            free(r);
            longname[120] = 0;
            snprintf(body, sizeof body, "{\"user_name\":\"%s\"}", longname);
            free(ask(s, "action", "set_settings", body, &st));
            check_true("120 characters: taken", st == CC_OK);
            free(ask(s, "action", "set_settings", "{\"user_name\":\"Ada Lovelace\"}", &st));
            free(before);
            before = read_all(FILE_, &n0);
        }
        free(ask(s, "action", "set_settings", "{\"theme\":\"dark\",\"text_size\":99}", &st));
        check_file("all or nothing: the file untouched after refusals", before);
        free(before);

        /* ---- FS1's recent files keep the settings (one writer) ---- */
        cc_recent_opened(s, "/m/one.ifc");
        size_t n;
        char *file = read_all(FILE_, &n);
        check_true("a recent file opened: the settings kept", file && strstr(file, "\"recent\":[\"/m/one.ifc\"]") && strstr(file, "\"theme\":\"light\"") &&
                                                               strstr(file, "\"user_name\":\"Ada Lovelace\""));
        free(file);
        free(ask(s, "action", "set_settings", "{\"theme\":\"grey\"}", &st));
        file = read_all(FILE_, &n);
        check_true("a setting changed: the recent files kept", file && strstr(file, "\"recent\":[\"/m/one.ifc\"]") && strstr(file, "\"theme\":\"grey\""));
        free(file);
        cc_session_free(s);
    }

    /* ---- members Rust does not know: kept ---- */
    {
        reset_dir();
        size_t gn;
        char *g = slurp("golden/settings", "unknown.json", &gn);
        write_all(FILE_, g);
        cc_session *s = session_on_file();
        free(ask(s, "action", "set_settings", "{\"user_name\":\"Ben\"}", &st));
        check_true("unknown members: set -> ok", st == CC_OK);
        char want[1024];
        const char *at = strstr(g, "\"user_name\":\"Jean Claude Van Damme\"");
        snprintf(want, sizeof want, "%.*s\"user_name\":\"Ben\"%s", (int)(at - g), g, at + strlen("\"user_name\":\"Jean Claude Van Damme\""));
        check_file("unknown members kept as they are, user_name replaced", want);
        free(g);
        cc_session_free(s);
    }

    /* ---- Rust's from_json on odd values (settings.rs tests) ---- */
    {
        cc_settings v;
        const char *odd = "{\"theme\": \"neon\", \"ui_font\": \"comic\", \"text_size\": 99}";
        cc_settings_parse(odd, strlen(odd), &v);
        check_true("odd: grey, roboto, 16", !strcmp(v.theme, "grey") && !strcmp(v.ui_font, "roboto") && v.text_size == 16.0f);
        check_true("\"not json\": -1 and the defaults", cc_settings_parse("not json", 8, &v) == -1 && v.memory_share == 0.8 && v.moving_fps == 30);
        cc_settings_parse("{\"memory_share\": 7}", 19, &v);
        check_true("memory_share 7 -> 1.0", v.memory_share == 1.0);
        cc_settings_parse("{\"memory_share\": \"x\"}", 21, &v);
        check_true("memory_share \"x\" -> the default", v.memory_share == 0.8);
        cc_settings_parse("{\"wgs84\": true}", 15, &v);
        check_true("0.14's wgs84 true -> coordinates wgs84", !strcmp(v.coordinates, "wgs84"));
        cc_settings_parse("{\"moving_fps\": 1000, \"user_name\": \"a\\tb\\nc\"}", 44, &v);
        check_true("moving_fps 1000 -> 240; control characters dropped", v.moving_fps == 240 && !strcmp(v.user_name, "abc"));
        const char *rep = "{\"coordinates\": \"epsg:+2056\", \"theme\": \"dark\", \"theme\": \"light\"}";
        cc_settings_parse(rep, strlen(rep), &v);
        check_true("epsg:+2056 (Rust's u16 parse); a repeated key: the last", !strcmp(v.coordinates, "epsg:+2056") && !strcmp(v.theme, "light"));
        char t[16];
        check_true("trigram Ada Lovelace -> AL", cc_settings_trigram("Ada Lovelace", t, sizeof t) && !strcmp(t, "AL"));
        check_true("trigram jean claude van damme -> JCV", cc_settings_trigram("jean claude van damme", t, sizeof t) && !strcmp(t, "JCV"));
        check_true("trigram pierre -> PIE", cc_settings_trigram("pierre", t, sizeof t) && !strcmp(t, "PIE"));
        check_true("trigram blank -> none", !cc_settings_trigram("  ", t, sizeof t));
        check_true("trigram élodie -> ÉLO", cc_settings_trigram("\xc3\xa9lodie", t, sizeof t) && !strcmp(t, "\xc3\x89LO"));
    }

    /* ---- a broken file: the defaults and a note, never a failure; left alone ---- */
    {
        reset_dir();
        write_all(FILE_, "not json {");
        cc_session *s = session_on_file();
        char *q = ask(s, "query", "settings", "", &st);
        char note[256];
        value_of(q, "note", note, sizeof note);
        check_true("broken file: query settings ok, a note", st == CC_OK && strcmp(note, "null") && strstr(note, "not JSON"));
        char *v = values(s), got[64];
        value_of(v, "theme", got, sizeof got);
        check_true("broken file: the defaults", !strcmp(got, "\"grey\""));
        free(ask(s, "action", "set_settings", "{\"theme\":\"dark\"}", &st));
        check_true("broken file: set_settings answers ok", st == CC_OK);
        check_file("broken file: left alone", "not json {");
        free(v);
        v = values(s);
        value_of(v, "theme", got, sizeof got);
        check_true("broken file: the change kept for the session", !strcmp(got, "\"dark\""));
        free(v), free(q);
        cc_session_free(s);
    }

    /* ---- no folder (the wasm module, the dev host): in memory, settings_load / settings_text ---- */
    {
        cc_session *s = cc_session_new(cc_platform_native());
        char *q = ask(s, "query", "settings", "", &st);
        char file[64];
        value_of(q, "file", file, sizeof file);
        check_true("no folder: file null", !strcmp(file, "null"));
        free(q);
        char *t0 = ask(s, "query", "settings_text", "", &st);
        check_true("no folder, nothing stored: settings_text null", st == CC_OK && !strcmp(t0, "null"));
        free(t0);
        free(ask(s, "action", "settings_load", "{\"text\":\"{\\\"user_name\\\":\\\"Zoé\\\",\\\"x_future\\\":[1]}\"}", &st));
        check_true("settings_load -> ok", st == CC_OK);
        char *se = ask(s, "query", "state", "", &st);
        check_true("settings_load: state.user from the stored text", strstr(se, "\"name\":\"Zoé\",\"trigram\":\"ZOÉ\"") != NULL);
        free(se);
        free(ask(s, "action", "set_settings", "{\"text_size\":14}", &st));
        char *t1 = ask(s, "query", "settings_text", "", &st);
        check_true("in memory: Rust's eleven fields, the unknown member kept",
                   !strcmp(t1, "\"{\\\"coordinates\\\":\\\"survey\\\",\\\"gpu_cull\\\":false,\\\"memory_share\\\":0.8,\\\"moving_fps\\\":30,\\\"moving_proxies\\\":true,"
                               "\\\"recent\\\":[],\\\"text_size\\\":14.0,\\\"theme\\\":\\\"grey\\\",\\\"ui_font\\\":\\\"roboto\\\",\\\"user_email\\\":\\\"\\\","
                               "\\\"user_name\\\":\\\"Zoé\\\",\\\"x_future\\\":[1]}\""));
        free(t1);
        free(ask(s, "action", "settings_load", "{\"text\":5}", &st));
        check_true("settings_load {text: 5} refused", st != CC_OK);
        cc_session_free(s);
    }

    /* ---- GPU1 (ADR 0083): the lab's 3D renderer, not one of Rust's fields: written only once set ---- */
    {
        reset_dir();
        cc_session *s = session_on_file();
        free(ask(s, "action", "set_settings", "{\"moving_fps\":60}", &st));
        size_t n = 0;
        char *f = read_all(FILE_, &n);
        check_true("renderer: a file written before it is set has no renderer member", st == CC_OK && f && !strstr(f, "renderer"));
        free(f);
        char *q = ask(s, "query", "settings", "", &st), g[64];
        value_of(q, "gpu_cull", g, sizeof g); /* applies (the first object of the answer) */
        check_true("renderer: auto by default; gpu_cull does not apply (WebGL2)", strstr(q, "\"renderer\":\"auto\"") && !strcmp(g, "false"));
        free(q);
        free(ask(s, "action", "set_settings", "{\"renderer\":\"webgpu\"}", &st));
        f = read_all(FILE_, &n);
        check_true("renderer: set to webgpu, written in its sorted place", st == CC_OK && f && strstr(f, "\"recent\":[],\"renderer\":\"webgpu\",\"text_size\":"));
        free(f);
        q = ask(s, "query", "settings", "", &st);
        value_of(q, "gpu_cull", g, sizeof g);
        check_true("renderer: webgpu read back; gpu_cull applies (compute shaders)", strstr(q, "\"renderer\":\"webgpu\"") && !strcmp(g, "true"));
        free(q);
        char *e = ask(s, "action", "set_settings", "{\"renderer\":\"vulkan\"}", &st);
        check_true("renderer: an unknown one refused", st != CC_OK && strstr(e, "`renderer`: auto, webgl2 or webgpu"));
        free(e);
        free(ask(s, "action", "set_settings", "{\"renderer\":\"auto\"}", &st));
        f = read_all(FILE_, &n);
        check_true("renderer: back to auto, written (the file had it)", st == CC_OK && f && strstr(f, "\"renderer\":\"auto\""));
        free(f);
        cc_settings d;
        cc_settings_default(&d);
        Buf b;
        buf_init(&b);
        cc_settings_to_json(&d, NULL, 0, &b);
        buf_append(&b, "", 1); /* a Buf is not NUL-terminated: one for strstr */
        check_true("renderer: not in Rust's to_json", b.p && !strstr(b.p, "renderer"));
        buf_free(&b);
        cc_session_free(s);
        reset_dir();
    }

    /* ---- SIZE3 (ADR 0078 amendment 5): the links' GPU budget in MB, not one of Rust's fields: written only once set.
     * Fails on the code before SIZE3: an unknown field. ---- */
    {
        reset_dir();
        cc_session *s = session_on_file();
        free(ask(s, "action", "set_settings", "{\"moving_fps\":60}", &st));
        size_t n = 0;
        char *f = read_all(FILE_, &n);
        check_true("link_gpu_mb: a file written before it is set has none", st == CC_OK && f && !strstr(f, "link_gpu_mb"));
        free(f);
        char *q = ask(s, "query", "settings", "", &st);
        check_true("link_gpu_mb: 768 by default, applies", strstr(q, "\"link_gpu_mb\":768") && strstr(q, "\"link_gpu_mb\":true") != NULL);
        free(q);
        free(ask(s, "action", "set_settings", "{\"link_gpu_mb\":512}", &st));
        f = read_all(FILE_, &n);
        check_true("link_gpu_mb: set to 512, written in its sorted place", st == CC_OK && f && strstr(f, "\"gpu_cull\":false,\"link_gpu_mb\":512,\"memory_share\":"));
        free(f);
        q = ask(s, "query", "settings", "", &st);
        check_true("link_gpu_mb: 512 read back", strstr(q, "\"link_gpu_mb\":512") != NULL);
        free(q);
        char *e = ask(s, "action", "set_settings", "{\"link_gpu_mb\":10}", &st);
        check_true("link_gpu_mb: under 64 refused", st != CC_OK && strstr(e, "`link_gpu_mb`: a whole number of MB from 64 to 65536"));
        free(e);
        cc_settings d;
        cc_settings_default(&d);
        Buf b;
        buf_init(&b);
        cc_settings_to_json(&d, NULL, 0, &b);
        buf_append(&b, "", 1);
        check_true("link_gpu_mb: not in Rust's to_json", b.p && !strstr(b.p, "link_gpu_mb"));
        buf_free(&b);
        cc_session_free(s);
        reset_dir();
    }

    /* ---- the actions, the capabilities, query about ---- */
    {
        cc_session *s = cc_session_new(cc_platform_native());
        char *a = ask(s, "query", "actions", "", &st);
        check_true("action settings enabled (ribbon.rs:591)", strstr(a, "{\"enabled\":true,\"label\":\"Settings…\",\"name\":\"settings\"") != NULL);
        check_true("action about enabled (ribbon.rs:591)", strstr(a, "{\"enabled\":true,\"label\":\"About Bimble…\",\"name\":\"about\"") != NULL);
        check_true("set_settings is not a ribbon action", !strstr(a, "set_settings"));
        free(a);
        free(ask(s, "action", "settings", "", &st));
        check_true("action settings runs", st == CC_OK);
        free(ask(s, "action", "about", "", &st));
        check_true("action about runs", st == CC_OK);
        char *c = ask(s, "query", "capabilities", "", &st);
        check_true("capabilities: about, settings, user true", strstr(c, "\"about\":true") && strstr(c, "\"settings\":true") && strstr(c, "\"user\":true"));
        free(c);
        char *ab = ask(s, "query", "about", "{\"graphics\":\"WebGL2 Test GPU\",\"commit\":\"abc123\"}", &st);
        check_true("query about: version, graphics, commit, platform", st == CC_OK && strstr(ab, "\"graphics\":\"WebGL2 Test GPU\"") &&
                                                                         strstr(ab, "\"commit\":\"abc123\"") && strstr(ab, "\"version\":\"0.1.0-ccore\""));
        check_true("query about: the bug report text (lib.rs:4089)",
                   strstr(ab, "\"text\":\"Bimble 0.1.0-ccore\\nCore: ") && strstr(ab, "abc123\\nGraphics: WebGL2 Test GPU\\nSystem: "));
        free(ab);
        ab = ask(s, "query", "about", "", &st);
        check_true("query about with no body: graphics null", st == CC_OK && strstr(ab, "\"graphics\":null"));
        free(ab);
        cc_session_free(s);
    }
    reset_dir();
    return report("settings");
}
