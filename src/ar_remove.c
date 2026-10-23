/* احذف — delete a file; a directory needs the explicit Arabic word دليل and is
 * never recursive: rmdir refuses a non-empty one by construction. */
#include "ar_util.h"
#include <stdlib.h>

static int fail(const char *p, const char *m) { fprintf(stderr, "احذف: %s ", m); ar_path(stderr, p); fputc('\n', stderr); return 1; }

int main(int argc, char **argv) {
    const char *p;
    if (argc < 2) { fprintf(stderr, "%s احذف مسار | احذف دليل مسار\n", AR_S7); return 2; }
    if (argc == 2) {
        p = argv[1];
        if (unlink(p) == 0) return 0;
        if (errno == EISDIR) {
            fprintf(stderr, "احذف: "); ar_path(stderr, p);
            fprintf(stderr, ": %s (%s احذف دليل)\n", AR_S3, AR_S7);
            return 1;
        }
        return fail(p, ar_errno_class(errno) == 2 ? AR_S2 : AR_S1);
    }
    if (argc == 3 && strcmp(argv[1], "دليل") == 0) {
        p = argv[2];
        if (rmdir(p) == 0) return 0;
        if (errno == ENOTEMPTY || errno == EEXIST) return fail(p, AR_S18);
        if (errno == ENOTDIR) return fail(p, AR_S10);
        return fail(p, ar_errno_class(errno) == 2 ? AR_S2 : AR_S1);
    }
    fprintf(stderr, "احذف: %s: %s\n", AR_S6, argv[1]);
    return 1;
}
