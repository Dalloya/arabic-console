#!/usr/bin/env python3
"""Small, inspectable Arabic joining engine."""
import argparse
from sakhr_forms import can_join_after, can_join_before

LETTERS={"hamza":"ء","alef":"ا","alef-with-madda":"آ","alef-with-hamza-above":"أ","alef-with-hamza-below":"إ","beh":"ب","teh":"ت","theh":"ث","jeem":"ج","hah":"ح","khah":"خ","dal":"د","thal":"ذ","reh":"ر","zain":"ز","seen":"س","sheen":"ش","sad":"ص","dad":"ض","tah":"ط","zah":"ظ","ain":"ع","ghain":"غ","feh":"ف","qaf":"ق","kaf":"ك","lam":"ل","meem":"م","noon":"ن","heh":"ه","waw":"و","waw-with-hamza":"ؤ","yeh":"ي","yeh-with-hamza":"ئ","teh-marbuta":"ة","alef-maksura":"ى"}
# Joining_Type values from Unicode ArabicShaping.txt (U=Non_Joining,
# R=Right_Joining, D=Dual_Joining, C=Join_Causing).  The table is deliberately keyed by
# codepoint rather than by our display names. The four-form ROM records now
# decide letter joining; this table remains for transparent marks and tatweel.
UNICODE_JOINING_TYPES = {
    0x621: "U", 0x622: "R", 0x623: "R", 0x624: "R", 0x625: "R",
    0x626: "D", 0x627: "R", 0x628: "D", 0x629: "R", 0x62A: "D",
    0x62B: "D", 0x62C: "D", 0x62D: "D", 0x62E: "D", 0x62F: "R",
    0x630: "R", 0x631: "R", 0x632: "R", 0x633: "D", 0x634: "D",
    0x635: "D", 0x636: "D", 0x637: "D", 0x638: "D", 0x639: "D",
    0x63A: "D", 0x640: "C", 0x641: "D", 0x642: "D", 0x643: "D", 0x644: "D",
    0x645: "D", 0x646: "D", 0x647: "D", 0x648: "R", 0x649: "R",
    0x64A: "D",
}
# The T rows below are the Arabic-script rows from Unicode 18.0.0
# DerivedJoiningType.txt.  Keep this as Joining_Type source data, rather than
# a hand-maintained "marks" list: TRANSPARENT is derived from its T values.
UNICODE_JOINING_TYPE_RANGES = (
    (0x0610, 0x061A, "T"), (0x061C, 0x061C, "T"),
    (0x064B, 0x065F, "T"), (0x0670, 0x0670, "T"),
    (0x06D6, 0x06DC, "T"), (0x06DF, 0x06E4, "T"),
    (0x06E7, 0x06E8, "T"), (0x06EA, 0x06ED, "T"),
    (0x070F, 0x070F, "T"), (0x0711, 0x0711, "T"),
    (0x0730, 0x074A, "T"), (0x07A6, 0x07B0, "T"),
    (0x07EB, 0x07F3, "T"), (0x07FD, 0x07FD, "T"),
    (0x0816, 0x0819, "T"), (0x081B, 0x0823, "T"),
    (0x0825, 0x0827, "T"), (0x0829, 0x082D, "T"),
    (0x0859, 0x085B, "T"), (0x0897, 0x089F, "T"),
    (0x08CA, 0x08E1, "T"), (0x08E3, 0x0902, "T"),
)
TRANSPARENT = frozenset(
    cp for first, last, joining in UNICODE_JOINING_TYPE_RANGES if joining == "T"
    for cp in range(first, last + 1)
)
RIGHT_ONLY=set(cp for cp, jt in UNICODE_JOINING_TYPES.items() if jt == "R")
NAME={v:k for k,v in LETTERS.items()}
# the lam-alef ligature. A LAM whose next joining neighbour is one of
# these ALEF variants collapses with it into ONE glyph and ONE cell.  The alef
# of the pair never joins forward, so only two forms exist: isolated (the
# letter before the lam does not join forward) and final (it does).  The
# glyphs were already in the font; the engine simply never formed them.
LAM_ALEF = {
    0x0622: ("lam-alef-madda", 0xFEF5, 0xFEF6),
    0x0623: ("lam-alef-hamza-above", 0xFEF7, 0xFEF8),
    0x0625: ("lam-alef-hamza-below", 0xFEF9, 0xFEFA),
    0x0627: ("lam-alef", 0xFEFB, 0xFEFC),
}
LAM_ALEF_NAME = {alef: name for alef, (name, _iso, _fina) in LAM_ALEF.items()}
LAM_ALEF_FORMS = {name: {"isol": iso, "fina": fina}
                  for _alef, (name, iso, fina) in LAM_ALEF.items()}

def joining_type(char):
    """Return Unicode Joining_Type for a supported Arabic character."""
    cp = ord(char)
    if cp in UNICODE_JOINING_TYPES:
        return UNICODE_JOINING_TYPES[cp]
    return "T" if cp in TRANSPARENT else "U"

def _previous_joining_letter(chars, index):
    index -= 1
    while index >= 0 and joining_type(chars[index]) == "T":
        index -= 1
    return chars[index] if index >= 0 else None

def _next_joining_letter(chars, index):
    index += 1
    while index < len(chars) and joining_type(chars[index]) == "T":
        index += 1
    return chars[index] if index < len(chars) else None

def lam_alef_index(chars, index):
    """Index of the ALEF variant that chars[index] ligates with, or None.
    chars[index] must be a LAM; transparent marks between the two are
    skipped, exactly as for joining neighbours."""
    if ord(chars[index]) != 0x644:
        return None
    j = index + 1
    while j < len(chars) and joining_type(chars[j]) == "T":
        j += 1
    return j if j < len(chars) and ord(chars[j]) in LAM_ALEF else None

def join_word(word):
    out=[]; chars=list(word); i=0
    while i<len(chars):
        if chars[i] not in NAME:
            # Transparent characters remain cells in the layout; shape.py
            # copies them directly and never asks the presentation-form table.
            out.append((chars[i], "transparent" if joining_type(chars[i]) == "T" else "unknown")); i+=1; continue
        alef=lam_alef_index(chars, i)
        if alef is not None:
      # LAM + ALEF variant become ONE ligature cell. The form
            # is chosen only by the letter before the lam; the pair itself
            # never joins forward (the alef is Right_Joining).  Marks between
            # the two stay transparent and keep their own cells.
            prev=_previous_joining_letter(chars, i)
            joins_prev=prev is not None and can_join_after(prev)
            out.append((LAM_ALEF_NAME[ord(chars[alef])], "fina" if joins_prev else "isol"))
            i+=1
            while i<alef:
                out.append((chars[i], "transparent")); i+=1
            i=alef+1
            continue
        prev=_previous_joining_letter(chars, i); nxt=_next_joining_letter(chars, i)
        # U+0640 TATWEEL is Joining_Type=C: it has no presentation form of
        # its own, but it joins the letters on both sides exactly as D does.
        joins_prev=can_join_before(chars[i]) and prev is not None and can_join_after(prev)
        joins_next=(can_join_after(chars[i]) and nxt is not None
                    and can_join_before(nxt))
        pos="medi" if joins_prev and joins_next else "fina" if joins_prev else "init" if joins_next else "isol"
        out.append((NAME[chars[i]],pos)); i+=1
    return out
def main():
    ap=argparse.ArgumentParser(); ap.add_argument("words", nargs="*"); ap.add_argument("--transparent", action="store_true"); a=ap.parse_args()
    if a.transparent:
        print("Unicode Joining_Type=T:", " ".join(f"U+{cp:04X}" for cp in sorted(TRANSPARENT)))
    for w in a.words: print(f"{w}: {join_word(w)}")
if __name__ == "__main__": main()
