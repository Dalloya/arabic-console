#ifndef AR_UTIL_H
#define AR_UTIL_H
/* Arabic utility contract: emit logical UTF-8 only. The wrapper owns shaping,
 * direction, cells and width. Output is one item per line, ASCII digits only,
 * with no controls other than LF. The frozen fonts do not contain U+0660-U+0669,
 * U+06F0-U+06F9, U+2014, U+2039 or U+203A. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *AR_S1 = "لا يوجد ملف أو دليل بهذا الاسم";
static const char *AR_S2 = "الإذن مرفوض";
static const char *AR_S3 = "إنه دليل";
static const char *AR_S4 = "الملف موجود";
static const char *AR_S5 = "معامل مفقود";
static const char *AR_S6 = "خيار غير معروف";
static const char *AR_S7 = "الاستخدام:";
static const char *AR_S8 = "تعذر الوصول إلى";
static const char *AR_S9 = "تعذر إنشاء الدليل";
static const char *AR_S10 = "ليس دليلاً";
static const char *AR_S11 = "تعذر فتح الملف";
static const char *AR_S12 = "تعذر قراءة الملف";
static const char *AR_S13 = "ليس نصاً";
static const char *AR_S14 = "حالة الخروج:";
static const char *AR_S15 = "الأوامر:";
static const char *AR_S16 = "ليس أمراً معروفاً";
static const char *AR_S17 = "للمساعدة:";
static const char *AR_S18 = "الدليل ليس فارغاً";
static const char *AR_S19 = "صيغة غير صحيحة";

static void ar_path(FILE *f, const char *p) { fputc('\'', f); fputs(p, f); fputc('\'', f); }
static int ar_usage(int argc, const char *name, const char *arg) {
    if (argc == 1) { fprintf(stderr, "%s %s\n", AR_S7, name); return 2; }
    fprintf(stderr, "%s: %s: %s\n", name, AR_S6, arg); return 2;
}
static int ar_errno_class(int e) {
    if (e == EACCES || e == EPERM) return 2;
    if (e == ENOTDIR) return 3;
    if (e == EEXIST) return 4;
    return 1;
}
#endif
