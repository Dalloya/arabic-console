/* ابحث — find by name (exact basename), optional type, from a start directory.
 * Paths are emitted intact, one per line; symlinks are not followed. */
#include "ar_util.h"
#include <dirent.h>
#include <stdlib.h>

static const char *want;
static int want_type;                    /* 0 any, 1 ملف, 2 دليل */
static int found;

static void walk(const char *dir, int depth) {
    DIR *d; struct dirent *e;
    if (depth > 64) return;
    d = opendir(dir);
    if (!d) { fprintf(stderr, "ابحث: %s ", AR_S8); ar_path(stderr, dir);
              fprintf(stderr, ": %s\n", AR_S1); return; }
    while ((e = readdir(d))) {
        char path[4096]; struct stat st; size_t dl;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        dl = strlen(dir);
        if (snprintf(path, sizeof path, "%s%s%s", dir,
                     (dl && dir[dl - 1] == '/') ? "" : "/", e->d_name) >= (int)sizeof path) continue;
        if (lstat(path, &st) != 0) continue;
        if (strcmp(e->d_name, want) == 0 &&
            (want_type == 0 || (want_type == 1 && S_ISREG(st.st_mode)) ||
             (want_type == 2 && S_ISDIR(st.st_mode)))) { puts(path); found = 1; }
        if (S_ISDIR(st.st_mode)) walk(path, depth + 1);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    int i = 1; const char *start = ".";
    struct stat st;
    if (argc < 2) { fprintf(stderr, "%s ابحث [ملف|دليل] الاسم [دليل-البداية]\n", AR_S7); return 2; }
    if (!strcmp(argv[1], "ملف")) { want_type = 1; i = 2; }
    else if (!strcmp(argv[1], "دليل")) { want_type = 2; i = 2; }
    if (i >= argc) { fprintf(stderr, "%s ابحث [ملف|دليل] الاسم [دليل-البداية]\n", AR_S7); return 2; }
    want = argv[i++];
    if (i < argc) start = argv[i++];
    if (i != argc) { fprintf(stderr, "ابحث: %s: %s\n", AR_S6, argv[i]); return 1; }
    if (lstat(start, &st) != 0) { fprintf(stderr, "ابحث: %s ", AR_S8); ar_path(stderr, start);
                                  fprintf(stderr, ": %s\n", AR_S1); return 1; }
    if (!S_ISDIR(st.st_mode)) { fprintf(stderr, "ابحث: "); ar_path(stderr, start);
                                fprintf(stderr, ": %s\n", AR_S10); return 1; }
    walk(start, 0);
    return found ? 0 : 1;
}
