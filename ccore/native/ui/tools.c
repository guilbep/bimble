/* The native app's 3D tools (ticket NUI1): Measure end to end (the core holds the tool, session/
 * measure.h: snapping to corners, edges, faces of the shown scene and its links; the app sends the
 * pointer's ray and draws what the core answers, as glviewport.ts does on its overlay), the section
 * box's handles (session/visibility.h), the view cube, and the views of `query views` (a plan or a
 * section: an orthographic camera and the view's box as the clip; the cut drawn as on paper is the web
 * lab's for now). The core's rays and boxes are in the open scene's frame (its origin, core_origin);
 * the renderer's in the app's scene frame (origin0): the two differ when the open model is empty. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "calls.h"

static void ray_json(App *a, float mx, float my, float tol_px, char *body, size_t n) {
    double o[3], d[3], o2[3], d2[3];
    float lx = mx - a->vx, ly = my - a->vy;
    nv_cam_ray(&a->cam, lx, ly, a->vw, a->vh, o, d);
    nv_cam_ray(&a->cam, lx + tol_px * a->scale, ly, a->vw, a->vh, o2, d2); /* the ray beside: the snap's tolerance */
    for (int i = 0; i < 3; i++) { /* to the core's frame */
        double k = a->origin0[i] - a->core_origin[i];
        o[i] += k, o2[i] += k;
    }
    snprintf(body, n, "{\"ray\":[%.3f,%.3f,%.3f,%.9f,%.9f,%.9f],\"ray_tol\":[%.3f,%.3f,%.3f,%.9f,%.9f,%.9f]}", o[0], o[1], o[2], d[0], d[1], d[2],
             o2[0], o2[1], o2[2], d2[0], d2[1], d2[2]);
}

void tool_measure_input(App *a, float mx, float my, int click) {
    char body[512];
    ray_json(a, mx, my, 12, body, sizeof body);
    Buf out;
    buf_init(&out);
    core_call(a, "tool", click ? "measure_click" : "measure_pointer", body, &out);
    json_parse(&a->measure, out.p ? out.p : "null", out.len ? out.len : 4);
    buf_free(&out);
}

/* A world point (an array of 3 numbers, mm) to screen px; 0 when behind the eye. */
static int to_screen(App *a, const JrVal *p, ImVec2 *out) {
    if (!p || p->type != JR_ARR || p->n != 3) return 0;
    double w[3], s[2];
    for (int i = 0; i < 3; i++) w[i] = p->items[i].num - a->origin0[i];
    if (!nv_cam_project(&a->cam, w, a->vw, a->vh, s)) return 0;
    out->x = a->vx + (float)s[0], out->y = a->vy + (float)s[1];
    return 1;
}

static void tag(App *a, ImDrawList *dl, ImVec2 p, const char *text) {
    float s = a->scale;
    igPushFont(a->f_mono, 13);
    ImVec2 ts = igCalcTextSize(text, NULL, false, -1);
    igPopFont();
    ts.x *= s, ts.y *= s;
    ImVec2 q = V2(p.x - ts.x / 2 - 6 * s, p.y - ts.y / 2 - 3 * s);
    ImDrawList_AddRectFilled(dl, q, V2(q.x + ts.x + 12 * s, q.y + ts.y + 6 * s), C_INK, 6 * s, 0);
    ImDrawList_AddText_FontPtr(dl, a->f_mono, 13 * s, V2(q.x + 6 * s, q.y + 3 * s), C_CREAM, text, NULL, 0, NULL);
}

void tool_measure_draw(App *a, ImDrawList *dl) {
    const JrVal *m = a->measure.root;
    if (!m || m->type != JR_OBJ || !a->measure_on) return;
    float s = a->scale;
    ImDrawList_PushClipRect(dl, V2(a->vx, a->vy), V2(a->vx + a->vw, a->vy + a->vh), true);
    const JrVal *items = jr_get(m, "items");
    for (size_t k = 0; items && items->type == JR_ARR && k < items->n; k++) {
        ImVec2 pa, pb;
        if (!to_screen(a, jr_get(&items->items[k], "a"), &pa) || !to_screen(a, jr_get(&items->items[k], "b"), &pb)) continue;
        ImDrawList_AddLine(dl, pa, pb, C_CLAY, 2 * s, 0);
        ImDrawList_AddCircleFilled(dl, pa, 3 * s, C_CLAY, 0), ImDrawList_AddCircleFilled(dl, pb, 3 * s, C_CLAY, 0);
        tag(a, dl, V2((pa.x + pb.x) / 2, (pa.y + pb.y) / 2), js(&items->items[k], "text"));
    }
    const JrVal *hover = jr_get(m, "hover");
    ImVec2 ph, pf;
    int has_h = hover && hover->type == JR_OBJ && to_screen(a, jr_get(hover, "at"), &ph);
    int has_f = to_screen(a, jr_get(m, "first"), &pf);
    if (has_f && has_h) {
        ImDrawList_AddLine(dl, pf, ph, C_INK, 1.5f * s, 0);
        const JrVal *pv = jr_get(m, "preview");
        if (pv && pv->type == JR_OBJ) tag(a, dl, V2((pf.x + ph.x) / 2, (pf.y + ph.y) / 2), js(pv, "text"));
    }
    if (has_h) {
        const char *snap = js(hover, "snap");
        if (!strcmp(snap, "corner")) ImDrawList_AddRect(dl, V2(ph.x - 5 * s, ph.y - 5 * s), V2(ph.x + 5 * s, ph.y + 5 * s), C_CLAY, 0, 2 * s, 0);
        else if (!strcmp(snap, "edge")) ImDrawList_AddTriangle(dl, V2(ph.x, ph.y - 6 * s), V2(ph.x - 6 * s, ph.y + 5 * s), V2(ph.x + 6 * s, ph.y + 5 * s), C_CLAY, 2 * s, 0);
        else ImDrawList_AddCircle(dl, ph, 5 * s, C_CLAY, 0, 2 * s, 0);
    }
    ImDrawList_PopClipRect(dl);
}

/* ---- the section box's handles: the core finds the face under the pointer (box_press), follows it
 * (box_move: a preview), keeps it (box_release) ---- */
int tool_box_input(App *a, const char *what, float mx, float my) {
    char body[512];
    ray_json(a, mx, my, 14, body, sizeof body);
    Buf out;
    buf_init(&out);
    int st = core_call(a, "tool", what, body, &out);
    int active = 0;
    if (st == CC_OK) {
        Json j = {0};
        json_parse(&j, out.p ? out.p : "null", out.len ? out.len : 4);
        active = jb(j.root, "active");
        const JrVal *box = j.root ? jr_get(j.root, "box") : NULL;
        const JrVal *mn = box && box->type == JR_OBJ ? jr_get(box, "min") : NULL, *mxv = box && box->type == JR_OBJ ? jr_get(box, "max") : NULL;
        if (mn && mxv && mn->type == JR_ARR && mxv->type == JR_ARR && mn->n == 3 && mxv->n == 3) { /* the preview while held */
            for (int i = 0; i < 3; i++) {
                double k = a->core_origin[i] - a->origin0[i];
                a->box_min[i] = mn->items[i].num + k, a->box_max[i] = mxv->items[i].num + k;
            }
            nv_render_set_clip(a->r, 1, a->box_min, a->box_max);
        }
        json_free(&j);
    }
    buf_free(&out);
    a->dirty = 1;
    return active;
}

void tool_box_draw(App *a, ImDrawList *dl) {
    if (!a->box_on || a->show != SHOW_VIEW || a->view_name[0]) return;
    float s = a->scale;
    const double *lo = a->box_min, *hi = a->box_max;
    ImDrawList_PushClipRect(dl, V2(a->vx, a->vy), V2(a->vx + a->vw, a->vy + a->vh), true);
    ImVec2 p[8];
    int ok[8];
    for (int k = 0; k < 8; k++) {
        double c[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]}, q[2];
        ok[k] = nv_cam_project(&a->cam, c, a->vw, a->vh, q);
        p[k] = V2(a->vx + (float)q[0], a->vy + (float)q[1]);
    }
    for (int k = 0; k < 8; k++) /* the 12 edges, in the reference colour */
        for (int b = 1; b < 8; b <<= 1)
            if (!(k & b) && ok[k] && ok[k | b]) ImDrawList_AddLine(dl, p[k], p[k | b], C_SLATE, 1.5f * s, 0);
    for (int f = 0; f < 6; f++) { /* a handle on each face's centre */
        double m[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}, q[2];
        m[f / 2] = f % 2 ? hi[f / 2] : lo[f / 2];
        if (!nv_cam_project(&a->cam, m, a->vw, a->vh, q)) continue;
        ImVec2 h = V2(a->vx + (float)q[0], a->vy + (float)q[1]);
        ImDrawList_AddCircleFilled(dl, h, 7 * s, C_SHEET, 0);
        ImDrawList_AddCircle(dl, h, 7 * s, C_INK, 0, 1.5f * s, 0);
    }
    ImDrawList_PopClipRect(dl);
}

/* ---- the view cube (ViewCube in glviewport.ts): the camera's turn shown on a cube at the view's top
 * right; a click on a face looks along it (Top: orthographic from above) ---- */
static const char *const FACE_KEY[6] = {"native.cube.right", "native.cube.left", "native.cube.back", "native.cube.front", "native.cube.top", "native.cube.bottom"};
int view_cube(App *a, ImDrawList *dl, float mx, float my, int click) {
    if (a->screen_kind != SCR_MAIN || a->show != SHOW_VIEW) return 0;
    float s = a->scale, R = 34 * s;
    float cx = a->vx + a->vw - 60 * s, cy = a->vy + 60 * s;
    double e[3];
    nv_camera c = a->cam;
    c.target[0] = c.target[1] = c.target[2] = 0, c.dist = 1;
    nv_cam_eye(&c, e); /* the direction to the eye */
    double f[3] = {-e[0], -e[1], -e[2]};
    double sx[3] = {f[1], -f[0], 0}, sl = sqrt(sx[0] * sx[0] + sx[1] * sx[1]);
    if (sl < 1e-9) sx[0] = -sin(c.yaw), sx[1] = cos(c.yaw), sl = 1;
    for (int i = 0; i < 3; i++) sx[i] /= sl;
    double u[3] = {sx[1] * f[2] - sx[2] * f[1], sx[2] * f[0] - sx[0] * f[2], sx[0] * f[1] - sx[1] * f[0]};
    static const int N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int hit_face = -1;
    float best = -1;
    for (int fc = 0; fc < 6; fc++) {
        double facing = -(N[fc][0] * f[0] + N[fc][1] * f[1] + N[fc][2] * f[2]);
        if (facing <= 0.05) continue;
        ImVec2 q[4];
        int ax = fc / 2, a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        for (int k = 0; k < 4; k++) {
            double p[3] = {0, 0, 0};
            p[ax] = N[fc][ax] * 0.5;
            p[a1] = (k == 1 || k == 2) ? 0.5 : -0.5;
            p[a2] = (k >= 2) ? 0.5 : -0.5;
            double px = p[0] * sx[0] + p[1] * sx[1] + p[2] * sx[2], py = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
            q[k] = V2(cx + (float)px * R * 1.4f, cy - (float)py * R * 1.4f);
        }
        /* inside the quad (convex): the same side of each edge */
        int in = 1;
        float sign = 0;
        for (int k = 0; k < 4; k++) {
            ImVec2 p0 = q[k], p1 = q[(k + 1) % 4];
            float cr = (p1.x - p0.x) * (my - p0.y) - (p1.y - p0.y) * (mx - p0.x);
            if (sign == 0) sign = cr;
            else if (cr * sign < 0) in = 0;
        }
        if (in && facing > best) best = (float)facing, hit_face = fc;
        if (dl) {
            ImU32 fill = in ? C_PINK : C_SHEET;
            ImDrawList_AddQuadFilled(dl, q[0], q[1], q[2], q[3], fill);
            ImDrawList_AddQuad(dl, q[0], q[1], q[2], q[3], C_STONE, 1 * s, 0);
            const char *lab = T(a, FACE_KEY[fc]);
            ImVec2 c0 = V2((q[0].x + q[2].x) / 2, (q[0].y + q[2].y) / 2);
            ImVec2 ts = igCalcTextSize(lab, NULL, false, -1);
            if (facing > 0.35) ImDrawList_AddText_FontPtr(dl, a->f_text, 11 * s, V2(c0.x - ts.x * 11 / 28 * s, c0.y - 6 * s), C_INK, lab, NULL, 0, NULL);
        }
    }
    if (!click || hit_face < 0) return 0;
    static const double YAW[6] = {0, NV_PI, NV_PI / 2, -NV_PI / 2, 0, 0};
    if (hit_face == 4) { view_top(a); return 1; }
    a->cam.yaw = hit_face == 5 ? a->cam.yaw : YAW[hit_face];
    a->cam.pitch = hit_face == 5 ? -1.5707 : 0;
    a->dirty = 1;
    return 1;
}

void view_top(App *a) {
    a->cam.yaw = -NV_PI / 2, a->cam.pitch = 1.5707;
    app_fit(a);
    a->cam.ortho = 1;
    a->dirty = 1;
}
void view_home(App *a) {
    a->cam.yaw = -55 * NV_PI / 180, a->cam.pitch = 28 * NV_PI / 180, a->cam.ortho = 0;
    app_fit(a);
}

/* A view of `query views` shown: the core opens its tab (calls.rs: tab open), its box comes from
 * query view_clip (a plan: from the level's floor to its top, a section: its depth); here an
 * orthographic camera looks along it and the box clips the 3D (the cut is not yet drawn as on paper).
 * NULL: the 3D view again. */
void view_set(App *a, const char *name) {
    const JrVal *v = a->views.root;
    if (v && v->type == JR_OBJ) v = jr_get(v, "views");
    const JrVal *row = NULL;
    for (size_t k = 0; name && v && v->type == JR_ARR && k < v->n; k++)
        if (!strcmp(js(&v->items[k], "name"), name)) row = &v->items[k];
    Buf b, o;
    buf_init(&b), buf_init(&o);
    if (!row) {
        a->view_name[0] = 0;
        view_home(a);
        core_visibility(a); /* the section box (if on) again */
        buf_free(&b), buf_free(&o);
        return;
    }
    buf_puts(&b, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":"), json_str(&b, js(row, "id")), buf_puts(&b, "}}");
    int st = core_call(a, "tab", NULL, buf_cstr(&b), &o);
    if (st != CC_OK) {
        snprintf(a->notice, sizeof a->notice, "%.*s", (int)(o.len < 300 ? o.len : 300), o.p ? o.p : ""), a->notice_t = app_now();
        buf_free(&b), buf_free(&o);
        return;
    }
    snprintf(a->view_name, sizeof a->view_name, "%s", name);
    Json c = {0};
    core_query(a, "view_clip", NULL, &c);
    const JrVal *clip = c.root ? jr_get(c.root, "clip") : NULL, *mn = clip ? jr_get(clip, "min") : NULL, *mx = clip ? jr_get(clip, "max") : NULL;
    const char *type = js(c.root, "type");
    if (mn && mx && mn->type == JR_ARR && mx->type == JR_ARR && mn->n == 3 && mx->n == 3) {
        double lo[3], hi[3];
        for (int i = 0; i < 3; i++) {
            double k = a->core_origin[i] - a->origin0[i];
            lo[i] = mn->items[i].num + k, hi[i] = mx->items[i].num + k;
        }
        nv_render_set_clip(a->r, 1, lo, hi);
        double slo[3], shi[3];
        int any = scene_box(a, slo, shi);
        for (int i = 0; i < 2 && any; i++) lo[i] = fmax(lo[i], slo[i]), hi[i] = fmin(hi[i], shi[i]); /* the fit: what the box holds */
        if (lo[0] < hi[0] && lo[1] < hi[1]) nv_cam_fit_box(&a->cam, lo, hi);
    }
    if (!strcmp(type, "section")) {
        const JrVal *d = jr_get(c.root, "dir");
        double dx = d && d->type == JR_ARR && d->n == 2 ? d->items[0].num : 0, dy = d && d->type == JR_ARR && d->n == 2 ? d->items[1].num : 1;
        a->cam.yaw = atan2(-dy, -dx), a->cam.pitch = 0; /* looking along the section's direction */
    } else a->cam.yaw = -NV_PI / 2, a->cam.pitch = 1.5707; /* a plan: from above */
    a->cam.ortho = 1;
    json_free(&c);
    buf_free(&b), buf_free(&o);
    a->dirty = 1;
}
