/* The lab's look in Dear ImGui (ticket NUI1): the brand tokens of the six themes (data/themes.json, from
 * web-ui/src/shell/theme.ts: the same values the lab's CSS takes), flat, the lab's fonts (the WOFF2
 * files of web-ui/src/brand, built into the binary, read by FreeType), latin and latin-ext merged
 * (French accents and more), the sizes scaled for HiDPI. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "embed.h"

Theme TH;

static ImU32 hex(const char *s, ImU32 dflt) {
    if (!s || s[0] != '#' || strlen(s) != 7) return dflt;
    unsigned v = (unsigned)strtoul(s + 1, NULL, 16);
    return 0xFF000000u | (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); /* #RRGGBB -> ABGR */
}

/* Theme `id` of themes.json (0: grey, the lab's default; 1: dark; or the order of the file). */
static const char *const THEME_IDS[] = {"grey", "dark", "light", "solarized-dark", "solarized-light", "high-contrast"};
void theme_set(App *a, int which) {
    a->theme = which;
    const nui_blob *b = nui_blob_get("themes.json");
    Arena ar;
    arena_init(&ar, 0);
    JrError e;
    const JrVal *v = b ? jr_parse(&ar, (const char *)b->p, b->n, &e) : NULL;
    const JrVal *base = v ? jr_get(v, "base") : NULL, *all = v ? jr_get(v, "themes") : NULL;
    const JrVal *t = all && which >= 0 && which < 6 ? jr_get(all, THEME_IDS[which]) : NULL;
#define TOK(field, name, dflt) TH.field = hex(*js(t, name) ? js(t, name) : js(base, name), dflt)
    TOK(cream, "cream", 0xFFEBF2F5u);
    TOK(sheet, "sheet", 0xFFFFFFFFu);
    TOK(sunk, "sunk", 0xFFDBE7EDu);
    TOK(ink, "ink", 0xFF141414u);
    TOK(stone, "stone", 0xFF4D575Cu);
    TOK(line, "line", 0xFFC8D6DDu);
    TOK(clay, "clay", 0xFF416AC0u);
    TOK(pink, "pink", 0xFFB6ACD7u);
    TOK(ochre, "ochre", 0xFF47B3D2u);
    TOK(slate, "slate", 0xFFA3886Eu);
    TOK(moss, "moss", 0xFF57876Au);
#undef TOK
    /* disabled text: stone halfway to the ground */
    ImU32 st = TH.stone, cr = TH.cream;
    TH.muted = 0xFF000000u | ((((st & 0xFF) + (cr & 0xFF)) / 2) & 0xFF) | (((((st >> 8) & 0xFF) + ((cr >> 8) & 0xFF)) / 2) << 8) |
               (((((st >> 16) & 0xFF) + ((cr >> 16) & 0xFF)) / 2) << 16);
    TH.view_bg = TH.sheet;
    arena_free(&ar);
    if (a->r) {
        ImVec4 c = col4(which == 0 ? 0xFFF5F7F7u : TH.sheet); /* the 3D view's ground (glviewport.ts: #F7F7F5 on the default) */
        float rgb[3] = {c.x, c.y, c.z};
        nv_render_set_background(a->r, rgb);
        a->dirty = 1;
    }
    if (igGetCurrentContext()) {
        ImGuiStyle *s = igGetStyle();
        ImVec4 *c = s->Colors;
        c[ImGuiCol_Text] = col4(C_INK), c[ImGuiCol_TextDisabled] = col4(C_MUTED);
        c[ImGuiCol_WindowBg] = col4(C_CREAM), c[ImGuiCol_ChildBg] = col4(C_CREAM), c[ImGuiCol_PopupBg] = col4(C_SHEET);
        c[ImGuiCol_Border] = col4(C_LINE), c[ImGuiCol_FrameBg] = col4(C_SHEET), c[ImGuiCol_FrameBgHovered] = col4(C_SUNK);
        c[ImGuiCol_FrameBgActive] = col4(C_SUNK), c[ImGuiCol_Button] = col4(C_SUNK), c[ImGuiCol_ButtonHovered] = col4(C_LINE);
        c[ImGuiCol_ButtonActive] = col4(C_LINE), c[ImGuiCol_Header] = col4(C_PINK), c[ImGuiCol_HeaderHovered] = col4(C_SUNK);
        c[ImGuiCol_HeaderActive] = col4(C_PINK), c[ImGuiCol_CheckMark] = col4(C_INK), c[ImGuiCol_Separator] = col4(C_LINE);
        c[ImGuiCol_TableHeaderBg] = col4(C_SUNK), c[ImGuiCol_TableBorderLight] = col4(C_LINE), c[ImGuiCol_TableBorderStrong] = col4(C_LINE);
        c[ImGuiCol_TableRowBg] = col4(C_SHEET), c[ImGuiCol_TableRowBgAlt] = col4(C_CREAM);
        c[ImGuiCol_TitleBg] = col4(C_SHEET), c[ImGuiCol_TitleBgActive] = col4(C_SHEET), c[ImGuiCol_TitleBgCollapsed] = col4(C_SHEET);
        c[ImGuiCol_ResizeGrip] = col4(C_SUNK), c[ImGuiCol_ResizeGripHovered] = col4(C_LINE), c[ImGuiCol_ResizeGripActive] = col4(C_STONE);
        c[ImGuiCol_ScrollbarBg] = col4(C_CREAM), c[ImGuiCol_ScrollbarGrab] = col4(C_LINE), c[ImGuiCol_TextSelectedBg] = col4(C_PINK);
        c[ImGuiCol_PlotHistogram] = col4(C_MOSS), c[ImGuiCol_NavCursor] = col4(C_INK), c[ImGuiCol_ModalWindowDimBg] = V4(0.08f, 0.08f, 0.08f, 0.35f);
    }
}
int theme_count(void) { return 6; }
const char *theme_id(int k) { return k >= 0 && k < 6 ? THEME_IDS[k] : "grey"; }

static int add(ImFontAtlas *at, const char *name, float px, ImFontConfig *base_cfg, int merge) {
    const nui_blob *b = nui_blob_get(name);
    if (!b) return -1;
    ImFontConfig *c = ImFontConfig_ImFontConfig();
    if (base_cfg) *c = *base_cfg;
    c->FontDataOwnedByAtlas = false; /* the bytes are the binary's */
    c->MergeMode = merge != 0;
    ImFont *f = ImFontAtlas_AddFontFromMemoryTTF(at, (void *)b->p, (int)b->n, px, c, NULL);
    ImFontConfig_destroy(c);
    return f ? 0 : -1;
}

static ImFont *font(ImFontAtlas *at, const char *family, const char *weight, float px) {
    char p1[128], p2[128];
    snprintf(p1, sizeof p1, "%s-latin-%s-normal.woff2", family, weight);
    snprintf(p2, sizeof p2, "%s-latin-ext-%s-normal.woff2", family, weight);
    if (add(at, p1, px, NULL, 0)) return NULL;
    ImFont *f = at->Fonts.Data[at->Fonts.Size - 1];
    add(at, p2, px, NULL, 1);
    /* what the family lacks (DM Mono has no U+2009, the thin space the core puts in 13 402 mm): Source Sans 3 */
    add(at, "source-sans-3-latin-ext-400-normal.woff2", px, NULL, 1);
    add(at, "source-sans-3-latin-400-normal.woff2", px, NULL, 1);
    return f;
}

int theme_apply(App *a) {
    ImGuiIO *io = igGetIO_Nil();
    ImFontAtlas *at = io->Fonts;
    a->f_text = font(at, "source-sans-3", "400", 14);
    a->f_semi = font(at, "source-sans-3", "600", 14);
    a->f_disp = font(at, "sora", "700", 14);
    a->f_mono = font(at, "dm-mono", "400", 13);
    int ok = a->f_text != NULL;
    if (!a->f_text) a->f_text = ImFontAtlas_AddFontDefault(at, NULL);
    if (!a->f_semi) a->f_semi = a->f_text;
    if (!a->f_disp) a->f_disp = a->f_semi;
    if (!a->f_mono) a->f_mono = a->f_text;
    io->FontDefault = a->f_text;
    ImGuiStyle *s = igGetStyle();
    /* tokens.css: radius sm 8, md 16, pill 999; space 4 8 12 16; the 32 px target */
    s->WindowRounding = 0, s->ChildRounding = 0, s->FrameRounding = 8, s->PopupRounding = 8;
    s->GrabRounding = 8, s->TabRounding = 999, s->ScrollbarRounding = 8;
    s->WindowBorderSize = 0, s->FrameBorderSize = 0, s->PopupBorderSize = 1;
    s->WindowPadding = V2(12, 10), s->FramePadding = V2(8, 5), s->ItemSpacing = V2(8, 6);
    s->IndentSpacing = 16, s->ScrollbarSize = 10;
    theme_set(a, a->theme);
    float k = a->offscreen ? a->scale : SDL_GetWindowDisplayScale(a->win);
    if (k > 0 && k != 1 && a->offscreen) {
        ImGuiStyle_ScaleAllSizes(s, k);
        s->FontScaleDpi = k;
    }
    return ok ? 0 : -1;
}
