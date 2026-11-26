/* See library.h. Values from crates/bimble-core/src/library.rs::library(). */
#include "library.h"
#include <string.h>
#include "guid.h"

#define SRC_C "EN 1992-1-1 Table 3.1; 25 kN/m\xc2\xb3 reinforced (EN 1991-1-1)"
#define SRC_S "EN 1993-1-1 Table 3.1 (t \xe2\x89\xa4 40 mm)"
#define SRC_GL "EN 14080 (E0,g,mean, \xcf\x81g,mean)"

static const cc_library_material LIB[] = {
    {"C20/25", CC_CAT_CONCRETE, 2500.0, 30000.0, 20.0, "fck", SRC_C},
    {"C25/30", CC_CAT_CONCRETE, 2500.0, 31000.0, 25.0, "fck", SRC_C},
    {"C30/37", CC_CAT_CONCRETE, 2500.0, 33000.0, 30.0, "fck", SRC_C},
    {"C35/45", CC_CAT_CONCRETE, 2500.0, 34000.0, 35.0, "fck", SRC_C},
    {"C40/50", CC_CAT_CONCRETE, 2500.0, 35000.0, 40.0, "fck", SRC_C},
    {"C45/55", CC_CAT_CONCRETE, 2500.0, 36000.0, 45.0, "fck", SRC_C},
    {"C50/60", CC_CAT_CONCRETE, 2500.0, 37000.0, 50.0, "fck", SRC_C},
    {"S235", CC_CAT_STEEL, 7850.0, 210000.0, 235.0, "fy", SRC_S},
    {"S275", CC_CAT_STEEL, 7850.0, 210000.0, 275.0, "fy", SRC_S},
    {"S355", CC_CAT_STEEL, 7850.0, 210000.0, 355.0, "fy", SRC_S},
    {"S460", CC_CAT_STEEL, 7850.0, 210000.0, 460.0, "fy", SRC_S},
    {"B500B", CC_CAT_STEEL, 7850.0, 205000.0, 500.0, "fyk", "SIA 262 (Es = 205 GPa)"},
    {"C24", CC_CAT_TIMBER, 420.0, 11000.0, 24.0, "fm,k", "EN 338 (\xcf\x81mean)"},
    {"GL24h", CC_CAT_TIMBER, 420.0, 11500.0, 24.0, "fm,g,k", SRC_GL},
    {"GL28h", CC_CAT_TIMBER, 460.0, 12600.0, 28.0, "fm,g,k", SRC_GL},
    {"GL32h", CC_CAT_TIMBER, 490.0, 14200.0, 32.0, "fm,g,k", SRC_GL},
};

const cc_library_material *cc_library(size_t *n) {
    *n = sizeof LIB / sizeof LIB[0];
    return LIB;
}

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static int word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '/';
}

/* Does the word s[0..n) (any case) start with the grade g (upper case already)? */
static int starts_with_grade(const char *s, size_t n, const char *g) {
    size_t gn = strlen(g);
    if (n < gn) return 0;
    for (size_t i = 0; i < gn; i++)
        if (upper(s[i]) != upper(g[i])) return 0;
    return 1;
}

const cc_library_material *cc_library_lookup(const char *name, size_t n) {
    const cc_library_material *best = NULL;
    size_t count = sizeof LIB / sizeof LIB[0];
    for (size_t k = 0; k < count; k++) {
        /* A whole word, or the start of one: both are "the word starts with the grade". Words
         * are runs of ASCII alphanumerics and '/'; any other byte (UTF-8 too) separates. */
        int found = 0;
        for (size_t i = 0; i < n && !found;) {
            if (!word_char(name[i])) { i++; continue; }
            size_t j = i;
            while (j < n && word_char(name[j])) j++;
            found = starts_with_grade(name + i, j - i, LIB[k].name);
            i = j;
        }
        if (found && (!best || strlen(LIB[k].name) >= strlen(best->name))) best = &LIB[k];
    }
    return best;
}

cc_material cc_library_to_material(const cc_library_material *lm) {
    cc_material m;
    memset(&m, 0, sizeof m);
    cc_guid g = cc_guid_new();
    memcpy(m.id.c, g.c, 22);
    m.name.p = lm->name;
    m.name.n = strlen(lm->name);
    m.category = lm->category;
    m.props.has_density = m.props.has_e = m.props.has_strength = 1;
    m.props.density_kg_m3 = lm->density_kg_m3;
    m.props.e_mpa = lm->e_mpa;
    m.props.strength_mpa = lm->strength_mpa;
    m.props.source.p = lm->source;
    m.props.source.n = strlen(lm->source);
    return m;
}
