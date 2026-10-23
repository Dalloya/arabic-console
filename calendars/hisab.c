/* layer1/hisab.c — Layer 1 proof: a real program with Arabic identifiers.
 *
 * It builds with gcc and prints Arabic.  Both halves of that sentence matter,
 * and they are independent:
 *
 *   identifiers — "المجموع", "العدد_الأول", "الناتج" are ordinary identifiers.
 *     gcc/clang accept them with NO locale set (they assume UTF-8 source);
 *     `nm hisab` shows the Arabic symbol name, which is checkable evidence.
 *   output — the console paints whatever codepoints the program writes, left to
 *     right, one codepoint per cell, with no shaping and no bidi.  So the
 *     program must hand it Presentation Forms-B in visual order.  The literals
 *     below come from layer1/shaped.h, generated from tools/shape.py — the same
 *     engine that decided the font's presentation-form table.
 *
 * Build:  python3 layer1/mkshaped.py && gcc -O2 -o hisab layer1/hisab.c
 * Run:    ./hisab            (no LANG, no locale, no desktop needed)
 */
#include <stdio.h>

#include "shaped.h"

/* وسائط ودالة بأسماء عربية */
int المجموع(int العدد_الأول, int العدد_الثاني)
{
    return العدد_الأول + العدد_الثاني;
}

int main(void)
{
    int الناتج = المجموع(20, 22);

    /* 1. The control line: base letters, logical order — what a stock Linux
     *    console does with the same text.  The font maps the base letters too,
     *    so this is not blank: it is isolated letters, reading the wrong way. */
    printf("UNSHAPED  %s = %d\n", "المجموع", الناتج);

    /* 2. The same sentence shaped.  The Arabic run is in visual order (from
     *    shaped.h) and sits at the RIGHT of the number: only the Arabic run is
     *    reversed, the digits are printed by printf and stay left-to-right. */
    printf("SHAPED    %d = %s\n", الناتج, SHAPED_LABEL);

    return 0;
}
