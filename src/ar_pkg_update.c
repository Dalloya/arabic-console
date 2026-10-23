#include "ar_pkg_common.h"
int main(void) {
    char *av[] = { "pacman", "--noconfirm", "-Syu", NULL };
    int rc;
    puts("حدّث الحزم: سيحدّث الحزم المثبتة.");
    if (!pkg_privileged()) {
        puts("حدّث الحزم: يلزم إذن الجذر.");
        puts("حدّث الحزم: تعذر تنفيذ العملية.");
        return pkg_finish(1);
    }
    if (!pkg_confirm()) {
        puts("حدّث الحزم: أُلغيت العملية.");
        return pkg_finish(1);
    }
    rc = pkg_run(av);
    puts(rc ? "حدّث الحزم: تعذر تنفيذ العملية." : "حدّث الحزم: تم تحديث الحزم.");
    return pkg_finish(rc);
}
