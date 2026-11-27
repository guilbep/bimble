/* The native app's scripted driver (ticket NUI1; spike NAT2's orbit-zoom preset): a script of steps,
 * one a line, run against the real app (offscreen): events are pushed into SDL's queue at their time
 * and go through the same path as a user's (the UI backend, then the view).
 *
 *   bimble --offscreen --script <file> --out <dir> [file]      (or --preset orbit-zoom)
 *
 * Steps (positions are fractions of the 3D view, 0..1; world points in mm):
 *   wait complete | wait <ms> | frames <n>        open done and every link drawn / a pause / n frames
 *   press fx fy | moveto fx fy ms | release         a held left drag (3 px events at 60 Hz)
 *   drag fx0 fy0 fx1 fy1 ms | rdrag ...             a drag (left: orbit; right: pan)
 *   click fx fy | rclick fx fy | wheel fx fy n      a left click (pick), right click, n wheel steps
 *   click_world x y z                               a click where that world point is on screen
 *   key <name> [ctrl] [shift] | action <name>       a key of the key table / an action (as the ribbon)
 *   open <path> | link_folder <path> | lang en|fr   File > Open, Link this folder, the language button
 *   eye <link name> on|off                          a link's eye
 *   shot <file.png> | report <file.txt>             the window's picture / the Performance report
 *   copy_report                                     Performance › Copy (the clipboard, and a file: its path checked)
 *   box_handle <face> <dx> <dy> <ms>                a drag of the section box's handle on that face (0 -x .. 5 +z), found where it is
 *                                                   drawn, by (dx, dy) logical px
 *   expect_box_moved <face> <mm>                    that face moved by at least mm since the last box_handle
 *   ctrl_click_world x y z | expect_nselected <n>     a Ctrl-click (the selection grows) / how many are selected
 *   expect_selected <GlobalId> | expect_link <name> | expect_none
 *   expect_measure <mm> <tol> | expect_hidden <n> | expect_parts <n> | expect_text <key> <text>
 *   quit
 * Out: <dir>/frames.csv (t_ms, kind, frame_ms, ui_ms, triangles, draws), inputs.csv (t_ms, what,
 * app_ms: event -> the frame's UI and core work done, frame_ms: event -> that frame's GPU fence),
 * memory.csv, summary.md, and each expectation's line on stderr ("drive: ok ..." / "drive: FAIL ..."). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ui.h"

static const char PRESET_ORBIT_ZOOM[] = /* the spike's gesture: the numbers compare with NAT2's table */
    "wait complete\nwait 300\nshot first.png\npress 0.35 0.5\nmoveto 0.60 0.5 3000\nwheel 0.5 0.5 1\nwait 100\nwheel 0.5 0.5 1\nwait 100\n"
    "wheel 0.5 0.5 1\nwait 100\nwheel 0.5 0.5 1\nwait 100\nwheel 0.5 0.5 1\nwait 100\nwheel 0.5 0.5 -1\nwait 100\nwheel 0.5 0.5 -1\nwait 100\n"
    "wheel 0.5 0.5 -1\nwait 100\nwheel 0.5 0.5 -1\nwait 100\nwheel 0.5 0.5 -1\nwait 100\nrelease\nwait 1000\nshot still.png\nclick 0.5 0.5\n"
    "frames 3\nshot picked.png\ndrag 0.4 0.5 0.55 0.5 1000\nwait 300\nreport report.txt\nquit\n";

static int fails;
static double step_t0 = -1, last_ev, press_x, press_y, cur_x, cur_y;
static int frames_left, held;
static double t_first_input;
static double box_before[2];
static nv_camera cam_before;

static void outpath(App *a, char *o, size_t n, const char *name) {
    if (name[0] == '/') snprintf(o, n, "%s", name);
    else snprintf(o, n, "%s/%s", a->out_dir ? a->out_dir : ".", name);
}

static int parse(App *a, const char *text) {
    const char *p = text;
    int cap = 64;
    a->steps = calloc((size_t)cap, sizeof *a->steps);
    while (*p && a->steps) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(n < 599 ? n : 599), p);
        p = e ? e + 1 : p + n;
        char *h = strchr(line, '#');
        if (h && (h == line || h[-1] == ' ')) *h = 0;
        char *c = line;
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) continue;
        if (a->nsteps == cap) {
            Step *ns = realloc(a->steps, (size_t)(cap *= 2) * sizeof *ns);
            if (!ns) return -1;
            a->steps = ns;
        }
        Step *s = &a->steps[a->nsteps++];
        memset(s, 0, sizeof *s);
        sscanf(c, "%23s", s->op);
        const char *rest = c + strlen(s->op);
        while (*rest == ' ') rest++;
        snprintf(s->arg, sizeof s->arg, "%s", rest);
    }
    return a->steps ? 0 : -1;
}

int drive_setup(App *a) {
    if (a->out_dir) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", a->out_dir);
        for (char *q = dir + 1; *q; q++) if (*q == '/') *q = 0, mkdir(dir, 0755), *q = '/';
        mkdir(dir, 0755);
    }
    char pth[1200];
    outpath(a, pth, sizeof pth, "frames.csv");
    a->flog = fopen(pth, "w");
    outpath(a, pth, sizeof pth, "inputs.csv");
    a->ilog = fopen(pth, "w");
    outpath(a, pth, sizeof pth, "memory.csv");
    a->mlog = fopen(pth, "w");
    if (!a->flog || !a->ilog || !a->mlog) { fprintf(stderr, "bimble: cannot write in %s\n", a->out_dir ? a->out_dir : "."); return -1; }
    fprintf(a->flog, "t_ms,kind,frame_ms,ui_ms,triangles,draws,tiles,logic_ms,acquire_ms,uibuild_ms,rec3d_ms,recui_ms,gpu_ms,missing,ties\n");
    fprintf(a->ilog, "t_ms,what,app_ms,frame_ms\n");
    fprintf(a->mlog, "t_ms,rss_mb,gpu_mb\n");
    if (a->preset) {
        if (strcmp(a->preset, "orbit-zoom")) { fprintf(stderr, "bimble: preset %s unknown (orbit-zoom)\n", a->preset); return -1; }
        return parse(a, PRESET_ORBIT_ZOOM);
    }
    FILE *f = fopen(a->script, "rb");
    if (!f) { fprintf(stderr, "bimble: cannot read %s\n", a->script); return -1; }
    char *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (!buf) return -1;
    buf[n] = 0;
    int rc = parse(a, buf);
    free(buf);
    return rc;
}

static void vxy(App *a, float fx, float fy, float *x, float *y) { *x = a->vx + fx * a->vw, *y = a->vy + fy * a->vh; }

static void push_mouse(App *a, Uint32 type, float x, float y, float wheel, int button) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    SDL_WindowID wid = SDL_GetWindowID(a->win);
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.windowID = wid, e.motion.x = x, e.motion.y = y, e.motion.state = held ? SDL_BUTTON_LMASK : 0;
        e.motion.xrel = (float)(x - cur_x), e.motion.yrel = (float)(y - cur_y);
    } else if (type == SDL_EVENT_MOUSE_WHEEL) e.wheel.windowID = wid, e.wheel.y = wheel, e.wheel.mouse_x = x, e.wheel.mouse_y = y, e.wheel.integer_y = (int)wheel;
    else e.button.windowID = wid, e.button.button = (Uint8)button, e.button.x = x, e.button.y = y, e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN, e.button.clicks = 1;
    cur_x = x, cur_y = y;
    SDL_PushEvent(&e);
}

static void push_key(App *a, SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN, e.key.windowID = SDL_GetWindowID(a->win), e.key.key = k, e.key.mod = mod, e.key.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP, e.key.down = false;
    SDL_PushEvent(&e);
}

static void expect(App *a, int ok, const char *what) {
    fprintf(stderr, "drive: %s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++, a->fails++;
}

static int world_xy(App *a, const char *arg, float *x, float *y) {
    double w[3], q[2];
    if (sscanf(arg, "%lf %lf %lf", &w[0], &w[1], &w[2]) != 3) return 0;
    for (int i = 0; i < 3; i++) w[i] -= a->origin0[i];
    if (!nv_cam_project(&a->cam, w, a->vw, a->vh, q)) return 0;
    *x = a->vx + (float)q[0], *y = a->vy + (float)q[1];
    return 1;
}

/* One step, or part of one; 1 when it is done. */
static int run_step(App *a, Step *s, double now) {
    const char *op = s->op, *arg = s->arg;
    float fx = 0, fy = 0, fx1 = 0, fy1 = 0, x, y;
    double ms = 0;
    if (!strcmp(op, "wait")) {
        if (!strcmp(arg, "complete")) return a->screen_kind == SCR_MAIN && a->t_complete > 0;
        return now - step_t0 >= atof(arg);
    }
    if (!strcmp(op, "frames")) {
        if (step_t0 == now) frames_left = atoi(arg);
        return frames_left <= 0;
    }
    if (!strcmp(op, "press")) {
        sscanf(arg, "%f %f", &fx, &fy), vxy(a, fx, fy, &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
        held = 1, press_x = x, press_y = y;
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0, SDL_BUTTON_LEFT);
        return 1;
    }
    if (!strcmp(op, "moveto") || !strcmp(op, "drag") || !strcmp(op, "rdrag")) {
        int full = strcmp(op, "moveto") != 0;
        double sx = press_x, sy = press_y;
        if (full) {
            sscanf(arg, "%f %f %f %f %lf", &fx, &fy, &fx1, &fy1, &ms);
            float x0, y0;
            vxy(a, fx, fy, &x0, &y0);
            sx = x0, sy = y0;
            if (step_t0 == now) {
                push_mouse(a, SDL_EVENT_MOUSE_MOTION, x0, y0, 0, 0);
                held = 1;
                push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0, 0, !strcmp(op, "rdrag") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT);
                last_ev = now;
                return 0;
            }
        } else sscanf(arg, "%f %f %lf", &fx1, &fy1, &ms);
        float tx, ty;
        vxy(a, fx1, fy1, &tx, &ty);
        if (now - last_ev >= 1000.0 / 60) { /* 60 Hz */
            double k = ms > 0 ? (now - step_t0) / ms : 1;
            if (k > 1) k = 1;
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(sx + (tx - sx) * k), (float)(sy + (ty - sy) * k), 0, 0);
            last_ev = now;
            if (k >= 1) {
                if (full) push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, tx, ty, 0, !strcmp(op, "rdrag") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT), held = 0;
                else press_x = tx, press_y = ty;
                return 1;
            }
        }
        return 0;
    }
    if (!strcmp(op, "box_handle")) { /* box_handle <face 0..5> <dx px> <dy px> <ms>: a held drag of that face's handle, found where it is drawn */
        int f = 0;
        float dx = 0, dy = 0;
        sscanf(arg, "%d %f %f %lf", &f, &dx, &dy, &ms);
        if (f < 0 || f > 5) f = 0;
        double m[3] = {(a->box_min[0] + a->box_max[0]) / 2, (a->box_min[1] + a->box_max[1]) / 2, (a->box_min[2] + a->box_max[2]) / 2}, q[2];
        m[f / 2] = f % 2 ? a->box_max[f / 2] : a->box_min[f / 2];
        if (step_t0 == now) {
            if (!a->box_on || !nv_cam_project(&a->cam, m, a->vw, a->vh, q)) { expect(a, 0, "box_handle: no section box, or its handle behind the eye"); return 1; }
            box_before[0] = a->box_min[f / 2], box_before[1] = a->box_max[f / 2];
            x = a->vx + (float)q[0], y = a->vy + (float)q[1];
            if (q[0] < 0 || q[1] < 0 || q[0] >= a->vw || q[1] >= a->vh) {
                char what[160]; snprintf(what, sizeof what, "box_handle: the handle of face %d is off the view (%.0f, %.0f)", f, q[0], q[1]);
                expect(a, 0, what);
                return 1;
            }
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            held = 1, press_x = x, press_y = y;
            push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0, SDL_BUTTON_LEFT);
            last_ev = now;
            return 0;
        }
        if (now - last_ev >= 1000.0 / 60) {
            double k = ms > 0 ? (now - step_t0) / ms : 1;
            if (k > 1) k = 1;
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(press_x + dx * a->scale * k), (float)(press_y + dy * a->scale * k), 0, 0);
            last_ev = now;
            if (k >= 1) { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(press_x + dx * a->scale), (float)(press_y + dy * a->scale), 0, SDL_BUTTON_LEFT), held = 0; return 1; }
        }
        return 0;
    }
    if (!strcmp(op, "release")) {
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)cur_x, (float)cur_y, 0, SDL_BUTTON_LEFT);
        held = 0;
        return 1;
    }
    if (!strcmp(op, "ctrl_click_world")) a->drive_ctrl = 1, op = "click_world"; /* a Ctrl-click: the selection grows */
    if (!strcmp(op, "click") || !strcmp(op, "rclick") || !strcmp(op, "click_world")) {
        if (!strcmp(op, "click_world")) {
            if (!world_xy(a, arg, &x, &y)) { expect(a, 0, "click_world: the point is not in front of the camera"); return 1; }
        } else sscanf(arg, "%f %f", &fx, &fy), vxy(a, fx, fy, &x, &y);
        int b = !strcmp(op, "rclick") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0, b);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 0, b);
        return 1;
    }
    if (!strcmp(op, "wheel")) {
        int n = 1;
        sscanf(arg, "%f %f %d", &fx, &fy, &n), vxy(a, fx, fy, &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_WHEEL, x, y, (float)n, 0);
        return 1;
    }
    if (!strcmp(op, "key")) {
        char k[32] = "";
        sscanf(arg, "%31s", k);
        SDL_Keymod mod = (strstr(arg, "ctrl") ? SDL_KMOD_CTRL : 0) | (strstr(arg, "shift") ? SDL_KMOD_SHIFT : 0);
        SDL_Keycode kc = !strcmp(k, "escape") ? SDLK_ESCAPE : !strcmp(k, "delete") ? SDLK_DELETE : (SDL_Keycode)(unsigned char)k[0];
        push_key(a, kc, mod);
        return 1;
    }
    if (!strcmp(op, "action")) { a->input_t = app_now(), snprintf(a->input_what, sizeof a->input_what, "action %.30s", arg); tool_run_action(a, arg); return 1; }
    if (!strcmp(op, "open")) { app_open(a, arg); return 1; }
    if (!strcmp(op, "view")) { a->input_t = app_now(), snprintf(a->input_what, sizeof a->input_what, "view"); view_set(a, *arg ? arg : NULL); return 1; }
    if (!strcmp(op, "show")) { /* show perf|keys|settings|file_menu|links|none: a panel or dialog as its control opens it */
        a->input_t = app_now(), snprintf(a->input_what, sizeof a->input_what, "show %.20s", arg);
        if (!strcmp(arg, "perf")) a->dialog = DLG_PERF;
        else if (!strcmp(arg, "keys")) a->dialog = DLG_KEYS;
        else if (!strcmp(arg, "settings")) tool_run_action(a, "settings");
        else if (!strcmp(arg, "file_menu")) a->file_menu = 1;
        else if (!strcmp(arg, "links")) a->links_panel = 1;
        else if (!strcmp(arg, "link_dialog")) tool_run_action(a, "link_folder");
        else a->dialog = DLG_NONE, a->file_menu = 0, a->links_panel = 0;
        return 1;
    }
    if (!strcmp(op, "call")) { /* call <op> <name> <json>: a core call as the page makes it (a test's setup) */
        char cop[24] = "", cname[64] = "";
        int used = 0;
        sscanf(arg, "%23s %63s %n", cop, cname, &used);
        Buf o;
        buf_init(&o);
        int st = core_call(a, cop, cname, arg + used, &o);
        if (st != 200) fprintf(stderr, "drive: call %s %s: %d %.*s\n", cop, cname, st, (int)(o.len < 300 ? o.len : 300), o.p ? o.p : "");
        buf_free(&o);
        a->links_synced = -1;
        return 1;
    }
    if (!strcmp(op, "link_folder")) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"path\":"), json_str(&b, arg), buf_puts(&b, "}");
        core_action(a, "link_folder", buf_cstr(&b));
        a->t_complete = 0;
        buf_free(&b);
        return 1;
    }
    if (!strcmp(op, "lang")) {
        a->input_t = app_now(), snprintf(a->input_what, sizeof a->input_what, "language");
        a->lang_en = !strcmp(arg, "en"), a->strings = a->lang_en ? a->strings_en : a->strings_fr;
        return 1;
    }
    if (!strcmp(op, "eye")) {
        char name[256] = "", on[8] = "";
        const char *sp = strrchr(arg, ' ');
        if (sp) snprintf(name, sizeof name, "%.*s", (int)(sp - arg), arg), snprintf(on, sizeof on, "%s", sp + 1);
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"op\":"), json_str(&b, strcmp(on, "off") ? "show" : "hide"), buf_puts(&b, ",\"name\":"), json_str(&b, name), buf_puts(&b, "}");
        a->input_t = app_now(), snprintf(a->input_what, sizeof a->input_what, "eye");
        core_action(a, "manage_links", buf_cstr(&b));
        buf_free(&b);
        return 1;
    }
    if (!strcmp(op, "shot")) {
        char p[1200];
        outpath(a, p, sizeof p, arg);
        app_shot(a, p);
        nv_render_stats st = nv_render_get_stats(a->r); /* NUI2: what that picture drew (per view: the A/B against what is visible) */
        fprintf(stderr, "drive: shot %s: %llu triangles, %u tiles, %u draws, %u commands\n", arg, (unsigned long long)st.tris, st.tiles, st.draws, st.commands);
        return 1;
    }
    if (!strcmp(op, "copy_report")) { /* Performance › Copy, as the button does: the clipboard and a file; stderr says where */
        a->notice_path[0] = 0;
        perf_copy(a);
        fprintf(stderr, "drive: copy_report: %s\n", a->notice);
        expect(a, a->notice_path[0] != 0, "copy_report: the report also written to a file");
        return 1;
    }
    if (!strcmp(op, "report")) {
        char p[1200];
        outpath(a, p, sizeof p, arg);
        Buf b;
        buf_init(&b);
        perf_report(a, &b);
        FILE *f = fopen(p, "w");
        if (f) fwrite(b.p, 1, b.len, f), fclose(f);
        buf_free(&b);
        return 1;
    }
    char what[700];
    if (!strcmp(op, "expect_selected")) {
        snprintf(what, sizeof what, "selected %s (got %s%s%s)", arg, a->sel_id[0] ? a->sel_id : "none", a->sel_link[0] ? " in " : "", a->sel_link);
        expect(a, a->selected && !strcmp(a->sel_id, arg), what);
        return 1;
    }
    if (!strcmp(op, "expect_link")) {
        snprintf(what, sizeof what, "selection in link %s (got %s)", arg, a->sel_link[0] ? a->sel_link : "the open model");
        expect(a, a->selected && !strcmp(a->sel_link, arg), what);
        return 1;
    }
    if (!strcmp(op, "expect_box_moved")) { /* expect_box_moved <face> <at least mm>: that face moved since the last box_handle */
        int f = 0;
        double mm = 1;
        sscanf(arg, "%d %lf", &f, &mm);
        if (f < 0 || f > 5) f = 0;
        double now_v = f % 2 ? a->box_max[f / 2] : a->box_min[f / 2], was = box_before[f % 2];
        snprintf(what, sizeof what, "the section box's face %d moved by at least %.0f mm (from %.1f to %.1f: %.1f mm; box on %d)", f, mm, was, now_v, fabs(now_v - was), a->box_on);
        expect(a, a->box_on && fabs(now_v - was) >= mm, what);
        return 1;
    }
    if (!strcmp(op, "click_cube")) { /* a click on the view cube's centre (its face toward the eye) */
        cam_before = a->cam;
        x = a->vx + a->vw - 60 * a->scale, y = a->vy + 60 * a->scale;
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0, SDL_BUTTON_LEFT);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 0, SDL_BUTTON_LEFT);
        return 1;
    }
    if (!strcmp(op, "expect_view_changed")) {
        int ch = fabs(a->cam.yaw - cam_before.yaw) > 1e-6 || fabs(a->cam.pitch - cam_before.pitch) > 1e-6 || a->cam.ortho != cam_before.ortho;
        snprintf(what, sizeof what, "the view cube turned the view (yaw %.3f -> %.3f, pitch %.3f -> %.3f)", cam_before.yaw, a->cam.yaw, cam_before.pitch, a->cam.pitch);
        expect(a, ch, what);
        return 1;
    }
    if (!strcmp(op, "expect_box")) { /* expect_box on|off */
        int want = !strcmp(arg, "on");
        snprintf(what, sizeof what, "the section box %s (got %s; min %.0f %.0f %.0f max %.0f %.0f %.0f)", arg, a->box_on ? "on" : "off", a->box_min[0], a->box_min[1], a->box_min[2],
                 a->box_max[0], a->box_max[1], a->box_max[2]);
        expect(a, a->box_on == want, what);
        return 1;
    }
    if (!strcmp(op, "look_box")) { /* look_box fx fy fz tx ty tz: the eye and the point looked at, fractions of the scene's box */
        double f[6] = {0.5, 0.5, 0.5, 0.9, 0.5, 0.5}, lo[3], hi[3];
        sscanf(arg, "%lf %lf %lf %lf %lf %lf", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]);
        if (scene_box(a, lo, hi)) {
            double e[3], t[3];
            for (int i = 0; i < 3; i++) e[i] = lo[i] + f[i] * (hi[i] - lo[i]), t[i] = lo[i] + f[3 + i] * (hi[i] - lo[i]);
            double d[3] = {e[0] - t[0], e[1] - t[1], e[2] - t[2]}, l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 1) {
                memcpy(a->cam.target, t, sizeof t);
                a->cam.dist = l, a->cam.yaw = atan2(d[1], d[0]), a->cam.pitch = asin(d[2] / l), a->cam.ortho = 0;
                a->dirty = 1;
            }
        }
        return 1;
    }
    if (!strcmp(op, "check_tiles")) { /* every leaf of every tile set: its elements' vertices inside the tile's box (the boxes the walker and
                                       * the occlusion trust); stderr: per set, the leaves and elements outside */
        tiles_check(a);
        return 1;
    }
    if (!strcmp(op, "expect_nselected")) {
        int want = atoi(arg), got = (a->selected != 0) + a->nmsel;
        snprintf(what, sizeof what, "%d elements selected (got %d)", want, got);
        expect(a, got == want, what);
        return 1;
    }
    if (!strcmp(op, "expect_none")) { expect(a, !a->selected, "nothing selected"); return 1; }
    if (!strcmp(op, "expect_measure")) {
        double mm = 0, tol = 1;
        sscanf(arg, "%lf %lf", &mm, &tol);
        const JrVal *it = a->measure.root ? jr_get(a->measure.root, "items") : NULL;
        double got = it && it->type == JR_ARR && it->n ? jnum(&it->items[it->n - 1], "value_mm", -1) : -1;
        snprintf(what, sizeof what, "measure %.1f mm +- %.1f (got %.3f: %s; tool %s)", mm, tol, got, it && it->n ? js(&it->items[it->n - 1], "text") : "none",
                 a->measure.root ? "answered" : a->measure.err[0] ? a->measure.err : "silent");
        expect(a, fabs(got - mm) <= tol, what);
        return 1;
    }
    if (!strcmp(op, "expect_hidden")) {
        int want = atoi(arg), hidden = 0; /* elements drawn hidden (their colour's alpha 0) are counted from the core's sets */
        Json v = {0};
        if (core_query(a, "visibility_sets", NULL, &v) == 0) {
            const JrVal *now = jr_get(v.root, "now"), *ids = now && now->type == JR_OBJ ? jr_get(now, "ids") : NULL;
            hidden = ids && ids->type == JR_ARR ? (int)ids->n : 0;
            if (jb(now, "isolate")) hidden = -hidden;
        }
        json_free(&v);
        snprintf(what, sizeof what, "hidden-for-now set %d (got %d; negative: isolated)", want, hidden);
        expect(a, hidden == want, what);
        return 1;
    }
    if (!strcmp(op, "expect_parts")) {
        int want = atoi(arg), det = 0;
        for (int i = 0; i < a->nlinks; i++) det += a->links_s[i].detail != NULL || (a->links_s[i].ts && nv_tileset_resident(a->links_s[i].ts) > 0);
        snprintf(what, sizeof what, "%d links drawn in detail (got %d of %d)", want, det, a->nlinks);
        expect(a, det == want, what);
        return 1;
    }
    if (!strcmp(op, "expect_text")) {
        char key[128] = "";
        sscanf(arg, "%127s", key);
        const char *want = arg + strlen(key);
        while (*want == ' ') want++;
        snprintf(what, sizeof what, "text %s = \"%s\" (got \"%s\")", key, want, T(a, key));
        expect(a, !strcmp(T(a, key), want), what);
        return 1;
    }
    if (!strcmp(op, "expect_report")) {
        Buf b;
        buf_init(&b);
        perf_report(a, &b);
        snprintf(what, sizeof what, "the report holds \"%s\"", arg);
        expect(a, strstr(buf_cstr(&b), arg) != NULL, what);
        buf_free(&b);
        return 1;
    }
    if (!strcmp(op, "quit")) { a->quit = 1; return 1; }
    if (!strcmp(op, "crash") && getenv("BIMBLE_TEST_CRASH")) { /* the crash handler's test (native_app_test, symbolize.sh): a fault on purpose */
        volatile int *volatile nowhere = NULL;
        return *nowhere;
    }
    snprintf(what, sizeof what, "unknown step \"%s\"", op);
    expect(a, 0, what);
    return 1;
}

void drive_step(App *a, double now) {
    while (a->steps && a->step_i < a->nsteps && !a->quit) {
        Step *s = &a->steps[a->step_i];
        if (step_t0 < 0) step_t0 = now, last_ev = 0;
        if (!t_first_input) t_first_input = now;
        if (!run_step(a, s, now)) return;
        a->step_i++, step_t0 = -1;
        if (strncmp(s->op, "expect", 6) && strcmp(s->op, "shot") && strcmp(s->op, "report")) return; /* one input step a frame */
    }
    if (a->steps && a->step_i >= a->nsteps) a->quit = 1;
}

/* The frame's log; the inputs answered by it (app_input_done). */
void drive_frame(App *a, double now, int drew3d) {
    if (frames_left > 0) frames_left--;
    if (a->rec_until && now >= a->rec_until) a->rec_until = 0, a->rec_done = 1;
    if (!a->flog) return;
    nv_render_stats st = nv_render_get_stats(a->r);
    fprintf(a->flog, "%.1f,%s,%.3f,%.3f,%llu,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n", now - a->t_start, drew3d ? (a->moving ? "moving" : "still") : "ui",
            drew3d ? a->frame_ms : a->gpu_ms, a->ui_ms, (unsigned long long)st.tris, st.draws, st.tiles, a->st_logic, a->st_acquire, a->st_ui, a->st_rec3d,
            a->st_recui, a->st_gpu, a->truth_missing, a->truth_missing >= 0 ? a->truth_ties : -1);
    if (now >= a->next_mem) {
        uint64_t peak = 0, rss = app_rss(&peak);
        fprintf(a->mlog, "%.0f,%.1f,%.1f\n", now - a->t_start, rss / 1e6, st.gpu_bytes / 1e6);
        a->next_mem = now + 250;
    }
}

/* An input answered: event -> the frame's fence; the slowest kept for the Performance panel. */
void app_input_done(App *a, double now) {
    double ms = now - a->input_t, app_ms = a->t_ui_done - a->input_t;
    if (app_ms < 0) app_ms = 0;
    if (a->ilog) fprintf(a->ilog, "%.1f,%s,%.3f,%.3f\n", a->input_t - a->t_start, a->input_what, app_ms, ms);
    slog_input(a, a->input_what, app_ms, ms);
    a->ninputs++;
    if (a->rec_until && ms > a->rec_slowest) a->rec_slowest = ms;
    int at = a->nslow;
    while (at > 0 && a->slow[at - 1].ms < ms) at--;
    if (at < 8) {
        int last = a->nslow < 8 ? a->nslow : 7;
        memmove(&a->slow[at + 1], &a->slow[at], (size_t)(last - at) * sizeof *a->slow);
        a->slow[at].ms = ms, a->slow[at].t = a->input_t;
        snprintf(a->slow[at].what, sizeof a->slow[at].what, "%s", a->input_what);
        if (a->nslow < 8) a->nslow++;
    }
    a->input_t = 0;
}

/* summary.md: the numbers of the run, from the logs. */
static int cmpd(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return a < b ? -1 : a > b; }
static void pct(double *v, int n, double *med, double *p95, double *mx) {
    *med = *p95 = *mx = NAN;
    if (!n) return;
    qsort(v, (size_t)n, sizeof *v, cmpd);
    *med = v[n / 2], *p95 = v[(int)(n * 0.95) < n ? (int)(n * 0.95) : n - 1], *mx = v[n - 1];
}

void drive_finish(App *a) {
    if (!a->flog) return;
    if (a->sdl_asserts) expect(a, 0, "no SDL assertion during the run (see stderr)");
    { Buf rb; buf_init(&rb); perf_report(a, &rb); buf_free(&rb); } /* the settings' line, as the report says it */
    fclose(a->flog), fclose(a->ilog), fclose(a->mlog);
    a->flog = a->ilog = a->mlog = NULL;
    char p[1200];
    double *mv = malloc(sizeof(double) * 200000), *st = malloc(sizeof(double) * 200000), *in = malloc(sizeof(double) * 200000),
           *ap = malloc(sizeof(double) * 200000);
    int nm = 0, ns = 0, ni = 0;
    if (!mv || !st || !in || !ap) { free(mv), free(st), free(in), free(ap); return; }
    /* NUI2: the moving frames' stages, triangles and tiles; the still frames' triangles; the truth test's missing ids */
    enum { NSTAGE = 6 };
    double *stg[NSTAGE], *tri_m = malloc(sizeof(double) * 200000), *tri_s = malloc(sizeof(double) * 200000), *til = malloc(sizeof(double) * 200000);
    for (int k = 0; k < NSTAGE; k++) stg[k] = malloc(sizeof(double) * 200000);
    long miss_total = 0, ties_total = 0;
    int miss_frames = 0, truth_frames = 0, nst = 0;
    outpath(a, p, sizeof p, "frames.csv");
    FILE *f = fopen(p, "r");
    char line[400];
    while (f && fgets(line, sizeof line, f)) {
        double t, fm;
        char kind[16];
        double um, tr, dr, ti, sv[NSTAGE];
        int miss = -1, tie = -1;
        if (sscanf(line, "%lf,%15[^,],%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d,%d", &t, kind, &fm, &um, &tr, &dr, &ti, &sv[0], &sv[1], &sv[2], &sv[3], &sv[4], &sv[5],
                   &miss, &tie) < 3) continue;
        if (tie > 0) ties_total += tie;
        if (!strcmp(kind, "moving") && nm < 200000) {
            for (int k = 0; k < NSTAGE && stg[k]; k++) stg[k][nm] = sv[k];
            if (tri_m) tri_m[nm] = tr;
            if (til) til[nm] = ti;
            mv[nm++] = fm, nst = nm;
        }
        if (!strcmp(kind, "still") && ns < 200000) {
            if (tri_s) tri_s[ns] = tr;
            st[ns++] = fm;
        }
        if (miss >= 0) truth_frames++, miss_total += miss, miss_frames += miss > 0;
    }
    if (f) fclose(f);
    outpath(a, p, sizeof p, "inputs.csv");
    f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        double t, am, fm;
        char what[64];
        if (sscanf(line, "%lf,%63[^,],%lf,%lf", &t, what, &am, &fm) != 4) continue;
        if (ni < 200000) in[ni] = fm, ap[ni] = am, ni++;
    }
    if (f) fclose(f);
    double m1, p1, x1, m2, p2, x2, m3, p3, x3, m4, p4, x4;
    pct(mv, nm, &m1, &p1, &x1), pct(st, ns, &m2, &p2, &x2), pct(in, ni, &m3, &p3, &x3), pct(ap, ni, &m4, &p4, &x4);
    uint64_t peak = 0, rss = app_rss(&peak);
    nv_render_stats rs = nv_render_get_stats(a->r);
    char load[128] = "?";
    f = fopen("/proc/loadavg", "r");
    if (f) { if (fgets(load, sizeof load, f)) load[strcspn(load, "\n")] = 0; fclose(f); }
    outpath(a, p, sizeof p, "summary.md");
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "# bimble (native) run\n\n");
        fprintf(f, "| | |\n|---|---|\n");
        fprintf(f, "| adapter | %s |\n| view | %u x %u px |\n", a->gpu_name ? a->gpu_name : "?", a->vpw, a->vph);
        fprintf(f, "| first picture (process start) | %.0f ms |\n| complete | %.0f ms |\n", a->t_first_picture ? a->t_first_picture - a->t_start : -1.0,
                a->t_complete ? a->t_complete - a->t_start : -1.0);
        fprintf(f, "| moving frame median / p95 | %.1f / %.1f ms (n %d) |\n| still frame median / p95 | %.1f / %.1f ms (n %d) |\n", m1, p1, nm, m2, p2, ns);
        fprintf(f, "| input -> frame median / p95 / max | %.1f / %.1f / %.1f ms (n %d) |\n", m3, p3, x3, ni);
        fprintf(f, "| input -> app work done median / p95 / max | %.1f / %.1f / %.1f ms |\n", m4, p4, x4);
        fprintf(f, "| memory peak / now | %.0f / %.0f MB |\n| GPU | %.0f MB |\n", peak / 1e6, rss / 1e6, rs.gpu_bytes / 1e6);
        fprintf(f, "| triangles / draws (last frame) | %llu / %u |\n| links drawn | %d |\n| /proc/loadavg | %s |\n", (unsigned long long)rs.tris, rs.draws, a->nlinks, load);
        fprintf(f, "| expectations failed | %d |\n| SDL assertions | %u |\n", fails, a->sdl_asserts);
        { /* NUI2 */
            double m5, p5, x5, m6, p6, x6, m7, p7, x7, sm[NSTAGE], sp[NSTAGE], sx[NSTAGE];
            pct(tri_m, tri_m ? nm : 0, &m5, &p5, &x5), pct(tri_s, tri_s ? ns : 0, &m6, &p6, &x6), pct(til, til ? nm : 0, &m7, &p7, &x7);
            for (int k = 0; k < NSTAGE; k++) pct(stg[k], stg[k] ? nst : 0, &sm[k], &sp[k], &sx[k]);
            double gmax = 0;
            outpath(a, p, sizeof p, "memory.csv");
            FILE *mf = fopen(p, "r");
            while (mf && fgets(line, sizeof line, mf)) {
                double t, r, g;
                if (sscanf(line, "%lf,%lf,%lf", &t, &r, &g) == 3 && g > gmax) gmax = g;
            }
            if (mf) fclose(mf);
            fprintf(f, "| triangles moving / still (median) | %.0f / %.0f |\n| tiles drawn moving (median) | %.0f |\n| GPU peak | %.0f MB |\n", m5, m6, m7, gmax);
            fprintf(f, "| moving frame by stage, median ms: logic / acquire / UI build / 3D record / UI record / GPU | %.2f / %.2f / %.2f / %.2f / %.2f / %.2f |\n", sm[0], sm[1], sm[2], sm[3],
                    sm[4], sm[5]);
            fprintf(f, "| truth test: frames, with ids missing, ids missing | %d / %d / %ld |\n| truth test: coplanar ties (another element at the same depth won the pixel) | %ld |\n", truth_frames, miss_frames, miss_total, ties_total);
            fprintf(f, "| settings | %s |\n", a->settings_line);
            if (truth_frames && miss_total) expect(a, 0, "the truth test: no element missing with occlusion");
        }
        fclose(f);
    }
    fprintf(stderr, "drive: summary: first %.0f ms, complete %.0f ms, moving %.1f/%.1f ms, still %.1f/%.1f ms, input->frame %.1f/%.1f/%.1f ms, app %.1f/%.1f/%.1f ms, peak %.0f MB, %d failed\n",
            a->t_first_picture ? a->t_first_picture - a->t_start : -1.0, a->t_complete ? a->t_complete - a->t_start : -1.0, m1, p1, m2, p2, m3, p3, x3, m4, p4, x4,
            peak / 1e6, fails);
    free(mv), free(st), free(in), free(ap), free(tri_m), free(tri_s), free(til);
    for (int k = 0; k < NSTAGE; k++) free(stg[k]);
}
