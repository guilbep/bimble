/* cc_material_classify (ifc_in.h), apart from the import so that the IFC display reader
 * (ifc_read.c) links without it: the viewer-only wasm leaves ifc_in.c out (build.sh wasm-view,
 * ticket W1). */
#include <stdlib.h>
#include <string.h>
#include "ifc_in.h"

static int has_sub(const char *s, size_t n, const char *w) {
    size_t k = strlen(w);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(s + i, w, k) == 0) return 1;
    return 0;
}

cc_category cc_material_classify(const char *name, size_t n) {
    char small[256];
    char *s = n < sizeof small ? small : malloc(n + 1);
    if (!s) return CC_CAT_OTHER;
    /* to_lowercase: ASCII, and Latin-1 capitals (U+00C0..U+00DE but U+00D7) in UTF-8. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        s[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (c == 0xC3 && i + 1 < n) {
            unsigned char d = (unsigned char)name[i + 1];
            s[i + 1] = (char)(d >= 0x80 && d <= 0x9E && d != 0x97 ? d + 0x20 : d);
            i++;
        }
    }
    s[n] = 0;
#define HAS(...) has_any(s, n, (const char *[]){__VA_ARGS__, NULL})
    cc_category cat;
    int c_dig = n > 1 && s[0] == 'c' && s[1] >= '0' && s[1] <= '9';
    int lc_dig = n > 2 && s[0] == 'l' && s[1] == 'c' && s[2] >= '0' && s[2] <= '9';
    int gl_dig = n > 2 && s[0] == 'g' && s[1] == 'l' && s[2] >= '0' && s[2] <= '9';
    int d_dig = n > 1 && s[0] == 'd' && s[1] >= '0' && s[1] <= '9';
    int s_dig = n > 1 && s[0] == 's' && s[1] >= '0' && s[1] <= '9';
    int slash = 0; /* memchr: the freestanding wasm32 shim has none */
    for (size_t i = 0; i < n; i++) slash |= s[i] == '/';
    struct { cc_category c; const char *w[12]; } t[] = {
        {CC_CAT_TIMBER, {"timber", "wood", "bois", "clt", "glulam", "lamell\xc3\xa9", "lamelle"}},
        {CC_CAT_STEEL, {"steel", "acier", "stahl", "metal", "m\xc3\xa9tal"}},
        {CC_CAT_MASONRY, {"brick", "brique", "masonry", "ma\xc3\xa7onnerie", "maconnerie", "block", "parpaing", "stone", "pierre"}},
        {CC_CAT_EARTH, {"earth", "terre", "soil", "ground", "terrain", "gravel", "gravier", "molasse"}},
        {CC_CAT_INSULATION, {"insulation", "isolation", "isolant", "mineral wool", "laine", "xps", "eps"}},
        {CC_CAT_GLASS, {"glass", "verre", "glazing", "vitrage"}},
    };
#undef HAS
    int any = 0;
    if (has_sub(s, n, "concrete") || has_sub(s, n, "b\xc3\xa9ton") || has_sub(s, n, "beton") ||
        (slash && (c_dig || lc_dig)))
        cat = CC_CAT_CONCRETE, any = 1;
    for (size_t k = 0; !any && k < sizeof t / sizeof *t; k++) {
        int hit = 0;
        for (int j = 0; t[k].w[j]; j++) hit |= has_sub(s, n, t[k].w[j]);
        if (t[k].c == CC_CAT_TIMBER) hit |= gl_dig || c_dig || d_dig;
        if (t[k].c == CC_CAT_STEEL) hit |= s_dig;
        if (hit) cat = t[k].c, any = 1;
    }
    if (!any) cat = CC_CAT_OTHER;
    if (s != small) free(s);
    return cat;
}
