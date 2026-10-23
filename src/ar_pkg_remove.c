#include "ar_pkg_common.h"
int main(int argc, char **argv) {
    char *av[128];
    int i, rc;
    if (argc < 2 || argc > 120) {
        puts("أزل حزمة: الاستخدام: أزل حزمة اسم الحزمة");
        puts("أزل حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(2);
    }
    for (i = 1; i < argc; ++i) if (!pkg_name_ok(argv[i])) {
        puts("أزل حزمة: اسم الحزمة غير صالح.");
        puts("أزل حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(2);
    }
    printf("أزل حزمة: سيزيل ");
    for (i = 1; i < argc; ++i) { if (i > 1) fputs("، ", stdout); pkg_external(argv[i]); }
    puts(".");
    if (!pkg_privileged()) {
        puts("أزل حزمة: يلزم إذن الجذر.");
        puts("أزل حزمة: تعذر تنفيذ العملية.");
        return pkg_finish(1);
    }
    if (!pkg_confirm()) {
        puts("أزل حزمة: أُلغيت العملية.");
        return pkg_finish(1);
    }
    av[0] = "pacman"; av[1] = "--noconfirm"; av[2] = "-R"; av[3] = "--";
    for (i = 1; i < argc; ++i) av[i + 3] = argv[i];
    av[argc + 3] = NULL;
    rc = pkg_run(av);
    puts(rc ? "أزل حزمة: تعذر تنفيذ العملية." : "أزل حزمة: تمت إزالة الحزم.");
    return pkg_finish(rc);
}
