/* اعرض الأذونات / اضبط الأذونات — one source, two builds (PERM_SET selects the
 * setter). The setter's interface is the closed Arabic word list; the numeric
 * octal form is accepted as data. */
#include "ar_util.h"
#include <stdlib.h>

#ifdef PERM_SET
static const char *nm = "اضبط الأذونات";
static const char *words[] = { "لا", "نفّذ", "اكتب", "اكتب+نفّذ",
                               "اقرأ", "اقرأ+نفّذ", "اقرأ+اكتب", "كل",
                               "اقرأ+اكتب+نفّذ" };
static int word_mode(const char *w) {
    int i;
    for (i = 0; i < 8; i++) if (strcmp(w, words[i]) == 0) return i;
    if (strcmp(w, words[8]) == 0) return 7;
    return -1;
}
static int digits_mode(const char *s, int *m) {
    int i, v = 0;
    if (strlen(s) != 3) return -1;
    for (i = 0; i < 3; i++) { if (s[i] < '0' || s[i] > '7') return -1; v = v * 8 + (s[i] - '0'); }
    *m = v; return 0;
}
int main(int argc, char **argv) {
    const char *p = argv[1]; int i, m = -1;
    if (argc != 3 && argc != 5) { fprintf(stderr, "%s %s مسار الصيغة\n", AR_S7, nm); return 2; }
    if (argc == 3) { if (digits_mode(argv[2], &m) != 0) m = -1; }
    else for (i = 0; i < 3; i++) {
        int w = word_mode(argv[2 + i]);
        if (w < 0) { m = -1; break; }
        if (m < 0) m = 0;
        m = m * 8 + w;
    }
    if (m < 0) { fprintf(stderr, "%s: %s: %s\n", nm, AR_S19, argv[2]); return 2; }
    if (chmod(p, (mode_t)m) != 0)
        return fprintf(stderr, "%s: %s ", nm, ar_errno_class(errno) == 2 ? AR_S2 : AR_S8), ar_path(stderr, p), fputc('\n', stderr), 1;
    return 0;
}
#else
static void row(const char *label, mode_t m) {
    printf("%s: %c%c%c\n", label, (m & 4) ? 'r' : '-', (m & 2) ? 'w' : '-', (m & 1) ? 'x' : '-');
}
int main(int argc, char **argv) {
    struct stat st;
    if (argc < 2) { fprintf(stderr, "%s اعرض الأذونات مسار\n", AR_S7); return 2; }
    if (argc > 2) { fprintf(stderr, "اعرض الأذونات: %s: %s\n", AR_S6, argv[2]); return 1; }
    if (lstat(argv[1], &st) != 0) {
        fprintf(stderr, "اعرض الأذونات: %s ", ar_errno_class(errno) == 2 ? AR_S2 : AR_S1);
        ar_path(stderr, argv[1]); fputc('\n', stderr); return 1;
    }
    row("المالك", (st.st_mode >> 6) & 7);
    row("المجموعة", (st.st_mode >> 3) & 7);
    row("الآخرون", st.st_mode & 7);
    return 0;
}
#endif
