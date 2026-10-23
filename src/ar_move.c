/* انقل / أعد التسمية — move or rename: one implementation, the name is argv[0]. */
#include "ar_util.h"
#include <stdlib.h>

static const char *nm;
static int fail(const char *p, const char *m) { fprintf(stderr, "%s: %s ", nm, m); ar_path(stderr, p); fputc('\n', stderr); return 1; }

int main(int argc, char **argv) {
    const char *s, *d; struct stat st;
    nm = strrchr(argv[0], '/'); nm = nm ? nm + 1 : argv[0];
    if (argc < 3) { fprintf(stderr, "%s %s المصدر الهدف\n", AR_S7, nm); return 2; }
    if (argc > 3) { fprintf(stderr, "%s: %s: %s\n", nm, AR_S6, argv[3]); return 1; }
    s = argv[1]; d = argv[2];
    if (lstat(s, &st) != 0) return fail(s, ar_errno_class(errno) == 2 ? AR_S2 : AR_S1);
    if (lstat(d, &st) == 0) return fail(d, AR_S4);          /* no silent overwrite */
    if (rename(s, d) == 0) return 0;
    switch (errno) {
    case EACCES: case EPERM: return fail(d, AR_S2);
    case ENOENT: return fail(s, AR_S1);
    case ENOTDIR: case EISDIR: return fail(d, AR_S3);
    default: return fail(d, AR_S8);                          /* e.g. EXDEV */
    }
}
