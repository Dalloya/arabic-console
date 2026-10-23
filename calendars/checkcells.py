#!/usr/bin/env python3
"""layer1/checkcells.py — predict the kernel console's cells for a program's
stdout, WITHOUT running it on the console.

Why this exists: the console resolves each codepoint through the font's own
Unicode table (conv_uni_to_pc in drivers/tty/vt/consolemap.c) and paints one
cell per codepoint, left to right.  So we can tell, from the font file alone,
(a) which glyph slot each printed codepoint selects, and (b) whether any
codepoint is missing from the font — a missing one is NOT blank on screen, the
kernel draws an inverse '?' (vc_get_glyph -> ucs_get_fallback -> U+FFFD -> '?'),
which is the classic "it rendered something, so it worked" trap.

usage: python3 layer1/checkcells.py [--font out/console-16x32.psfu] FILE_OR_TEXT
       (a program's stdout:  ./hisab | python3 layer1/checkcells.py -)
"""
import argparse
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read_psfu(path):
    """Return (width, height, glyphs, unicode->slot map).

    Table format: per-slot records of UTF-8 codepoints, alternate mappings for
    one slot separated by 0xFE, each record terminated by 0xFF — the same
    reading `tools/console_font.py:parse_unicode_table` uses, and the one
    `psfgettable` agrees with.
    """
    d = pathlib.Path(path).read_bytes()
    magic, version, headersize, flags, count, charsize, height, width = \
        struct.unpack("<4s7I", d[:32])
    if magic != b"\x72\xb5\x4a\x86":
        raise SystemExit(f"{path}: not a PSF2/PSFU file")
    glyph_end = headersize + count * charsize
    glyphs = [d[headersize + i * charsize:headersize + (i + 1) * charsize]
              for i in range(count)]
    uni = {}
    offset = glyph_end
    if flags & 1:
        for slot in range(count):
            end = d.find(b"\xff", offset)
            if end < 0:
                raise SystemExit(f"{path}: incomplete Unicode table at {slot}")
            for alt in d[offset:end].split(b"\xfe"):
                for ch in alt.decode("utf-8"):
                    uni.setdefault(ord(ch), slot)
            offset = end + 1
    return width, height, glyphs, uni


def art(glyph, width, height, cellw=16):
    """ASCII art of one glyph."""
    stride = (width + 7) // 8
    lines = []
    for y in range(height):
        row = glyph[y * stride:(y + 1) * stride]
        bits = "".join(format(b, "08b") for b in row)[:width]
        lines.append(bits.replace("0", ".").replace("1", "#"))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="file, '-' for stdin, or text with --text")
    ap.add_argument("--font", default=str(ROOT / "out" / "console-16x32.psfu"))
    ap.add_argument("--text", action="store_true", help="treat source as the text")
    ap.add_argument("--art", action="store_true", help="ASCII-render the cells")
    args = ap.parse_args()

    if args.text:
        data = args.source
    elif args.source == "-":
        data = sys.stdin.buffer.read().decode("utf-8", "surrogateescape")
    else:
        data = pathlib.Path(args.source).read_text(encoding="utf-8",
                                                   errors="surrogateescape")

    width, height, glyphs, uni = read_psfu(args.font)
    print(f"font {args.font}: {len(glyphs)} glyphs, {width}x{height} px, "
          f"{len(uni)} codepoints mapped")

    missing = 0
    for line in data.splitlines():
        cells = []
        for ch in line:
            cp = ord(ch)
            slot = uni.get(cp)
            if slot is None:
                missing += 1
                cells.append(f"!!MISSING(U+{cp:04X})")
            else:
                cells.append(f"slot{slot:3d}:U+{cp:04X}")
        print(f"\nline: {line!r}")
        print("  cells: " + " ".join(cells))

    if args.art:
        for label, text in (("base letters", "سلام"), ("shaped", "سلام")):
            cps = ([ord(c) for c in text] if label == "base letters"
                   else [0xFEE1, 0xFEFC, 0xFEB3])
            rows = [""] * height
            for cp in cps:
                slot = uni.get(cp)
                if slot is None:
                    continue
                g = art(glyphs[slot], width, height)
                for y in range(height):
                    rows[y] += g[y] + "."
            print(f"\n--- {label} ({' '.join('U+%04X' % c for c in cps)}) ---")
            for r in rows:
                print("  " + r)

    print(f"\n{missing} codepoint(s) not in the font "
          f"(those paint an inverse '?' on the real console)")


if __name__ == "__main__":
    main()
