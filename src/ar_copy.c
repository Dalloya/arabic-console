/* انسخ — copy a file: contents and metadata, never a silent overwrite. */
#include "ar_util.h"
#include <fcntl.h>
#include <stdlib.h>

static int fail(const char *p, const char *m) { fprintf(stderr, "انسخ: %s ", m); ar_path(stderr, p); fputc('\n', stderr); return 1; }

int main(int argc, char **argv) {
    const char *s, *d; struct stat st, dst; mode_t mode;
    struct timespec ts[2]; int in, out, rc = 0;
    unsigned char b[65536]; ssize_t n, off;
    if (argc < 3) { fprintf(stderr, "%s انسخ المصدر الهدف\n", AR_S7); return 2; }
    if (argc > 3) { fprintf(stderr, "انسخ: %s: %s\n", AR_S6, argv[3]); return 1; }
    s = argv[1]; d = argv[2];
    if (lstat(s, &st) != 0) return fail(s, ar_errno_class(errno) == 2 ? AR_S2 : AR_S1);
    if (S_ISDIR(st.st_mode)) return fail(s, AR_S3);
    if (lstat(d, &dst) == 0) return fail(d, AR_S4);         /* no silent overwrite */
    mode = st.st_mode & 07777; ts[0] = st.st_atim; ts[1] = st.st_mtim;
    in = open(s, O_RDONLY);
    if (in < 0) return fail(s, ar_errno_class(errno) == 2 ? AR_S2 : AR_S11);
    out = open(d, O_WRONLY | O_CREAT | O_EXCL, mode);       /* O_EXCL: the guard is atomic */
    if (out < 0) { int c = ar_errno_class(errno); close(in);
                   return fail(d, c == 4 ? AR_S4 : (c == 2 ? AR_S2 : AR_S9)); }
    while ((n = read(in, b, sizeof b)) > 0) {
        for (off = 0; off < n; ) { ssize_t w = write(out, b + off, (size_t)(n - off));
                                   if (w <= 0) { rc = 1; break; } off += w; }
        if (rc) break;
    }
    if (n < 0) rc = 1;
    if (!rc && fchmod(out, mode) != 0) rc = 1;              /* metadata: exact mode */
    if (!rc && futimens(out, ts) != 0) rc = 1;              /* metadata: times */
    if (close(out) != 0) rc = 1;
    close(in);
    if (rc) { unlink(d); return fail(d, AR_S12); }
    return 0;
}
