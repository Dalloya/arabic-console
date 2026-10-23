#include "ar_pkg_common.h"
int main(int argc, char **argv) {
    char *av[128];
    int i, rc;
    if (argc < 2 || argc > 120) {
        puts("ثبّت حزمة: الاستخدام: ثبّت حزمة اسم الحزمة");
        puts("ثبّت حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(2);
    }
    for (i = 1; i < argc; ++i) if (!pkg_name_ok(argv[i])) {
        puts("ثبّت حزمة: اسم الحزمة غير صالح.");
        puts("ثبّت حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(2);
    }
    printf("ثبّت حزمة: سيُثبّت ");
    for (i = 1; i < argc; ++i) { if (i > 1) fputs("، ", stdout); pkg_external(argv[i]); }
    puts(".");
    if (!pkg_privileged()) {
        puts("ثبّت حزمة: يلزم إذن الجذر.");
        puts("ثبّت حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(1);
    }
    if (!pkg_confirm()) {
        puts("ثبّت حزمة: أُلغيت العملية.");
        return pkg_finish(1);
    }
    av[0] = "pacman"; av[1] = "--noconfirm"; av[2] = "--needed";
    av[3] = "-S"; av[4] = "--";
    for (i = 1; i < argc; ++i) av[i + 4] = argv[i];
    av[argc + 4] = NULL;
    rc = pkg_run(av);
    puts(rc ? "ثبّت حزمة: تعذر تنفيذ العملية." : "ثبّت حزمة: تم تثبيت الحزم.");
    return pkg_finish(rc);
}
