/* Native dump of the slab mesh: `slab local|scene`, same text as slab_dump.rs. */
#include "slab.h"
#include <stdio.h>
#include <string.h>

static unsigned b32(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
static unsigned long long b64(double d) { unsigned long long u; memcpy(&u, &d, 8); return u; }

int main(int argc, char **argv) {
    static const double outline[4][2] = {{0.0, 0.0}, {6000.0, 0.0}, {6000.0, 4000.0}, {0.0, 4000.0}};
    static const double at[3] = {1000.0, 2000.0, 3000.0};
    static SlabMesh m;
    int scene, i, k;
    if (argc < 2 || (strcmp(argv[1], "local") && strcmp(argv[1], "scene"))) {
        fprintf(stderr, "usage: slab local|scene\n");
        return 2;
    }
    scene = !strcmp(argv[1], "scene");
    if (slab_mesh(outline, 4, 220.0, at, &m, scene)) {
        fprintf(stderr, "slab does not mesh\n");
        return 1;
    }
    if (scene)
        printf("origin %016llx %016llx %016llx\n", b64(m.origin[0]), b64(m.origin[1]), b64(m.origin[2]));
    for (i = 0; i < m.nv; i++)
        printf("v %08x %08x %08x\n", b32(m.pos[i][0]), b32(m.pos[i][1]), b32(m.pos[i][2]));
    for (i = 0; i < m.nt; i++)
        printf("i %u %u %u\n", (unsigned)m.idx[3 * i], (unsigned)m.idx[3 * i + 1], (unsigned)m.idx[3 * i + 2]);
    for (i = 0; i < m.ne; i++) {
        printf("e");
        for (k = 0; k < 6; k++) printf(" %08x", b32(m.edges[i][k]));
        printf("\n");
    }
    if (scene)
        for (i = 0; i < m.ne; i++) {
            printf("n");
            for (k = 0; k < 8; k++) printf(" %d", m.normals[i][k]);
            printf("\n");
        }
    return 0;
}
