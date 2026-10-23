/* فتّش — search text inside files: line number and the matching line, UTF-8
 * logical output, one match per line so RTL cannot be broken by the tool.
 * Plain substring (no regex), binary files skipped, symlinks not followed. */
#include "ar_util.h"
#include <dirent.h>
#include <stdlib.h>

static const char *pat;
static int found;

static void scan(const char *path) {
    FILE *f; char probe[512], *line = NULL; size_t cap = 0, got, k; ssize_t n; unsigned long ln = 0;
    f = fopen(path, "r");
    if (!f) { fprintf(stderr, "فتّش: %s ", AR_S11); ar_path(stderr, path); fputc('\n', stderr); return; }
    got = fread(probe, 1, sizeof probe, f);
    for (k = 0; k < got; k++) if (probe[k] == '\0') { fclose(f); return; }   /* binary */
    rewind(f);
    while ((n = getline(&line, &cap, f)) > 0) {
        size_t L = (size_t)n;
        ln++;
        if (L && line[L - 1] == '\n') line[--L] = '\0';
        if (strstr(line, pat)) { printf("%s:%lu: %s\n", path, ln, line); found = 1; }
    }
    free(line); fclose(f);
}

static void walk(const char *dir, int depth) {
    DIR *d; struct dirent *e;
    if (depth > 64) return;
    d = opendir(dir);
    if (!d) { fprintf(stderr, "فتّش: %s ", AR_S8); ar_path(stderr, dir);
              fprintf(stderr, ": %s\n", AR_S1); return; }
    while ((e = readdir(d))) {
        char path[4096]; struct stat st; size_t dl;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        dl = strlen(dir);
        if (snprintf(path, sizeof path, "%s%s%s", dir,
                     (dl && dir[dl - 1] == '/') ? "" : "/", e->d_name) >= (int)sizeof path) continue;
        if (lstat(path, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) scan(path);
        else if (S_ISDIR(st.st_mode)) walk(path, depth + 1);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *start = ".";
    struct stat st;
    if (argc < 2) { fprintf(stderr, "%s فتّش النص [المسار]\n", AR_S7); return 2; }
    if (argc > 3) { fprintf(stderr, "فتّش: %s: %s\n", AR_S6, argv[3]); return 1; }
    pat = argv[1];
    if (argc == 3) start = argv[2];
    if (lstat(start, &st) != 0) { fprintf(stderr, "فتّش: %s ", AR_S8); ar_path(stderr, start);
                                  fprintf(stderr, ": %s\n", AR_S1); return 1; }
    if (S_ISREG(st.st_mode)) scan(start);
    else if (S_ISDIR(st.st_mode)) walk(start, 0);
    else { fprintf(stderr, "فتّش: "); ar_path(stderr, start); fprintf(stderr, ": %s\n", AR_S10); return 1; }
    return found ? 0 : 1;
}
