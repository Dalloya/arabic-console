#ifndef AR_PKG_COMMON_H
#define AR_PKG_COMMON_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void pkg_status(int rc) {
    printf("حالة الخروج: %d\n", rc);
}

static int pkg_finish(int rc) {
    pkg_status(rc);
    return rc;
}

static int pkg_confirm(void) {
    char line[64];
    fputs("هل توافق على تنفيذ العملية؟ أجب بنعم أو لا.\n", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 0;
    line[strcspn(line, "\r\n")] = '\0';
    return strcmp(line, "نعم") == 0;
}

static int pkg_privileged(void) {
    pid_t p;
    int st;
    if (geteuid() == 0) return 1;
    fflush(stdout);
    p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        FILE *null = freopen("/dev/null", "w", stdout);
        (void)null;
        null = freopen("/dev/null", "w", stderr);
        (void)null;
        execlp("sudo", "sudo", "-n", "-v", (char *)0);
        _exit(127);
    }
    while (waitpid(p, &st, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int pkg_name_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (!*p || *p == '-') return 0;
    for (; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '@' || *p == '.' ||
               *p == '_' || *p == '+' || *p == '-')) return 0;
    return 1;
}

static int pkg_run(char *const pacman_argv[]) {
    pid_t p = fork();
    int st;
    fflush(stdout);
    if (p < 0) return 1;
    if (p == 0) {
        FILE *null = freopen("/dev/null", "w", stdout);
        (void)null;
        null = freopen("/dev/null", "w", stderr);
        (void)null;
        if (geteuid() == 0) execvp("pacman", pacman_argv);
        else {
            char *sudo_argv[128];
            size_t i = 0;
            sudo_argv[i++] = "sudo";
            sudo_argv[i++] = "-n";
            while (pacman_argv[i - 2] && i < 126) {
                sudo_argv[i] = pacman_argv[i - 2];
                ++i;
            }
            sudo_argv[i] = NULL;
            execvp("sudo", sudo_argv);
        }
        _exit(127);
    }
    while (waitpid(p, &st, 0) < 0) if (errno != EINTR) return 1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

static void pkg_external(const char *s) {
    printf("'%s'", s);
}

#endif
