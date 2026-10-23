#!/usr/bin/env python3
"""Shape Arabic into the reversed presentation-form stream used by the console."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from join import LETTERS, LAM_ALEF_FORMS, join_word
from sakhr_forms import record

def shape_with_records(text):
    # A console line is laid out left-to-right, so reverse both levels: words
    # and the joined glyphs within each word.
    out = []
    for wi, word in enumerate(reversed(text.split(" "))):
        if wi:
            out.append((0x20, None))
        for name, pos in reversed(join_word(word)):
            if pos in ("unknown", "transparent"):
                out.append((ord(name), None))
            elif name in LAM_ALEF_FORMS:
        # the lam-alef ligature has its own glyphs; they are
                # not per-letter presentation forms of either member.
                out.append((LAM_ALEF_FORMS[name][pos], None))
            else:
                form = record(LETTERS[name], pos)
                if not form or not form["unicode"]:
                    raise ValueError(f"Sakhr has no {pos} form for {name}")
                out.append((int(form["unicode"][2:], 16), form))
    return out


def shape(text):
    return [cp for cp, _form in shape_with_records(text)]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", action="store_true", help="write bare UTF-8")
    ap.add_argument("--codes", action="store_true", help="show only U+ codepoints")
    ap.add_argument("--records", action="store_true", help="show selected ROM slot and attribute")
    ap.add_argument("text", nargs="*", help="Arabic text; stdin when omitted")
    args = ap.parse_args()
    text = " ".join(args.text) if args.text else sys.stdin.read().rstrip("\n")
    records = shape_with_records(text)
    cps = [cp for cp, _form in records]
    encoded = "".join(chr(cp) for cp in cps).encode("utf-8")
    if args.bytes:
        sys.stdout.buffer.write(encoded + b"\n")
    elif args.records:
        print(" ".join(f"U+{cp:04X}@{form['slot']},{form['attribute']}" if form else f"U+{cp:04X}@-"
                       for cp, form in records))
    elif args.codes:
        print(" ".join(f"U+{cp:04X}" for cp in cps))
    else:
        print("CODES: " + " ".join(f"U+{cp:04X}" for cp in cps))
        print("BYTES: " + " ".join(f"{byte:02X}" for byte in encoded))

if __name__ == "__main__":
    main()
