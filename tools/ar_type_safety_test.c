/* Direct regression for an editor state which normal input used to leave stale. */
#define main ar_type_program_main
#include "ar_type.c"
#undef main

int main(void) {
    struct Line before[MAX_LINES];
    size_t before_count;
    lines[0].cp[0] = 0x0645; lines[0].cp[1] = 0x0644;
    lines[0].len = 2; line_count = 1;
    cursor_line = 0; cursor_col = 1;
    anchor_line = 0; anchor_col = 99; /* impossible stored selection endpoint */
    memcpy(before, lines, sizeof(lines)); before_count = line_count;
    printf("before len=%zu cps=%x,%x\n", lines[0].len, lines[0].cp[0], lines[0].cp[1]);
    if (delete_selection() != 0 || line_count != before_count ||
        memcmp(before, lines, sizeof(lines)) != 0) return 1;
    printf("after  len=%zu cps=%x,%x (unchanged)\n", lines[0].len, lines[0].cp[0], lines[0].cp[1]);
    return 0;
}
