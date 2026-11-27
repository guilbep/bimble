/* --compare (ticket NUI2): the same offscreen script under several settings sets, one run each (this binary again, so
 * each starts cold in its own process), then their summaries side by side.
 *   bimble --offscreen ... --script s.txt --out dir --compare "renderer_path=parts" --compare "draw_visible=false" file
 * Each --compare is a set of key=value separated by commas (each one a --set); "-" the defaults. The runs go in
 * dir/run-1, dir/run-2, ...; the table on stdout and in dir/compare.md. The runs follow one another: the first warms
 * the system's file cache for the next (said in the table). */
#define _GNU_SOURCE
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ui.h"

extern char **environ;

static int rows_of(const char *path, char rows[64][2][200]) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < 64) {
        if (line[0] != '|' || !strncmp(line, "|---", 4) || !strncmp(line, "| |", 3)) continue;
        char *a = line + 1, *b = strchr(a, '|');
        if (!b) continue;
        *b = 0;
        char *c = b + 1, *d = strrchr(c, '|');
        if (d) *d = 0;
        while (*a == ' ') a++;
        while (*c == ' ') c++;
        for (char *e = a + strlen(a); e > a && e[-1] == ' ';) *--e = 0;
        for (char *e = c + strlen(c); e > c && (e[-1] == ' ' || e[-1] == '\n');) *--e = 0;
        snprintf(rows[n][0], 200, "%s", a), snprintf(rows[n][1], 200, "%s", c);
        n++;
    }
    fclose(f);
    return n;
}

int compare_main(int argc, char **argv) {
    const char *sets[8];
    int nsets = 0;
    const char *out = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compare") && i + 1 < argc && nsets < 8) sets[nsets++] = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    if (nsets < 1) return 2;
    mkdir(out, 0755);
    char runs[8][1200];
    for (int s = 0; s < nsets; s++) {
        snprintf(runs[s], sizeof runs[s], "%s/run-%d", out, s + 1);
        char **av = calloc((size_t)argc + 40, sizeof *av);
        int n = 0;
        av[n++] = argv[0];
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--compare") || !strcmp(argv[i], "--out")) { i++; continue; }
            av[n++] = argv[i];
        }
        av[n++] = "--out", av[n++] = runs[s];
        char *copy = strdup(sets[s]), *save = NULL;
        for (char *kv = strtok_r(copy, ",", &save); kv && n < argc + 36; kv = strtok_r(NULL, ",", &save))
            if (strcmp(kv, "-")) av[n++] = "--set", av[n++] = kv;
        /* the file stays last (the parser takes any non-option as the file) */
        for (int i = 1; i < n; i++)
            if (av[i][0] != '-' && (i == 1 || (strcmp(av[i - 1], "--set") && strcmp(av[i - 1], "--out") && strcmp(av[i - 1], "--script") &&
                                               strcmp(av[i - 1], "--size") && strcmp(av[i - 1], "--scale") && strcmp(av[i - 1], "--preset") &&
                                               strcmp(av[i - 1], "--config-dir") && strcmp(av[i - 1], "--resize-script") && strcmp(av[i - 1], "--loaders") &&
                                               strcmp(av[i - 1], "--edges") && strcmp(av[i - 1], "--quit-after") && strcmp(av[i - 1], "--shot") &&
                                               strcmp(av[i - 1], "--link-folder")))) {
                char *f = av[i];
                memmove(&av[i], &av[i + 1], (size_t)(n - i - 1) * sizeof *av);
                av[n - 1] = f;
                break;
            }
        fprintf(stderr, "compare: run %d of %d: %s\n", s + 1, nsets, sets[s]);
        pid_t pid;
        int st = 0;
        if (posix_spawn(&pid, "/proc/self/exe", NULL, NULL, av, environ) == 0) waitpid(pid, &st, 0);
        else fprintf(stderr, "compare: cannot run %s\n", argv[0]);
        free(copy), free(av);
    }
    static char rows[8][64][2][200];
    int nr[8];
    for (int s = 0; s < nsets; s++) {
        char p[1300];
        snprintf(p, sizeof p, "%s/summary.md", runs[s]);
        nr[s] = rows_of(p, rows[s]);
    }
    char mp[1300];
    snprintf(mp, sizeof mp, "%s/compare.md", out);
    FILE *f = fopen(mp, "w");
    FILE *o[2] = {stdout, f};
    for (int w = 0; w < 2; w++) {
        if (!o[w]) continue;
        fprintf(o[w], "| | ");
        for (int s = 0; s < nsets; s++) fprintf(o[w], "%s | ", sets[s]);
        fprintf(o[w], "\n|---|");
        for (int s = 0; s < nsets; s++) fprintf(o[w], "---|");
        fprintf(o[w], "\n");
        for (int r = 0; r < nr[0]; r++) {
            fprintf(o[w], "| %s | ", rows[0][r][0]);
            for (int s = 0; s < nsets; s++) {
                const char *v = "-";
                for (int k = 0; k < nr[s]; k++) if (!strcmp(rows[s][k][0], rows[0][r][0])) v = rows[s][k][1];
                fprintf(o[w], "%s | ", v);
            }
            fprintf(o[w], "\n");
        }
        fprintf(o[w], "\n(runs one after the other, in this order: the first warms the system's file cache)\n");
    }
    if (f) fclose(f);
    return 0;
}
