#!/usr/bin/env python3
"""The مساعدة table's MEANING, as an independent predicate.

The owner decided on 2026-10-13 to refresh the WO82 expectation to the
console's current command surface and its justification hint.  The case must
assert what the help table IS: every expected entry present
as a COMPLETE entry (its name and its description, one row) in the shape the
product actually emits.  This module is that predicate, used by
tools/console_verify.sh (both the --smoke path and the battery) and by the host
fixture harness tools/t82_help_case_tests.sh, so the two can never drift.

Reference model (independent of the product's current output):
  * the shaped letter stream comes from tools/shape.py + tools/join.py — the
    project's shaper, the same model the console's own case pins were measured
    from (WO73/WO75);
  * a word is a sequence of maximal ASCII (< 0x80) and Arabic runs, emitted
    last-to-first, an ASCII run keeping its own left-to-right order
    (tools/ar_type.c:339-346, task 77 finding 3) — so "Ctrl+S" reads forward;
  * a mark's CELL comes from the console's own table `dedicated_mark_cells`
    (tools/ar_type.c:158-163): terminal U+E000+i / connector U+E010+i for the
    marks U+064B..U+0652, U+0670, where connector marks follow initial/medial
    letters only (the rule tools/ar_type_test.py:122 measures against the real
    binary).  That mapping is draw_codepoint()'s paint-time job, not the
    shaper's, so it is re-derived here rather than imported from the product.
    This models rendering; by the owner's 2026-10-14 decision the keyboard
    delivers Sakhr's eight marks on Shift, while U+0670 remains a font cell;
  * a typed shadda immediately BEFORE one of the six vowels is ONE stacked cell
    carried by the shadda (tools/ar_type.c:164-173, task 54); no help entry
    uses one today, and the predicate still reproduces the rule so a future
    entry cannot silently pass a two-cell row where the console draws one.

Predicate: for each expected entry, SOME emitted row must carry EXACTLY that
entry's inked cells - same cell list, no extra ink, in column order.  Not a
subsequence: a bag-of-letters row, a partial row, an unshaped row, a wrapped
row, or a row carrying two entries cannot satisfy one.  Blank cells are absent
from the readout (no ink), so spaces are dropped on both sides.

usage:
  python3 tools/help_entries.py --entries FILE --rows FILE
  python3 tools/help_entries.py --entries FILE --rows FILE --quiet
exit 0 iff every entry is present; prints one verdict line per entry.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from join import join_word          # noqa: E402
from shape import shape             # noqa: E402

# tools/ar_type.c:158-163 `dedicated_mark_cells` (logical, terminal, connector).
_MARK_CELLS = [(cp, 0xE000 + i, 0xE010 + i) for i, cp in enumerate(range(0x064B, 0x0653))]
_MARK_CELLS.append((0x0670, 0xE008, 0xE018))
DRAW_MARKS = {cp: term for cp, term, _ in _MARK_CELLS}
CONNECTOR_MARKS = {cp: conn for cp, _, conn in _MARK_CELLS}
ALL_MARK_CELLS = set(DRAW_MARKS) | set(CONNECTOR_MARKS)
PRIVATE_MARK_CELLS = set(DRAW_MARKS.values()) | set(CONNECTOR_MARKS.values())
# WO53/WO54: a typed shadda immediately BEFORE a vowel coalesces into one cell
# (tools/ar_type.c:164-173 shadda_combo_vowel/shadda_combo_codepoint).  No help
# entry uses one today; the predicate reproduces the rule so a future entry
# cannot silently pass a two-cell row.
_SHADDA = 0x0651
_COMBO_VOWELS = (0x064E, 0x064F, 0x0650, 0x064B, 0x064C, 0x064D)
_PLAIN_COMBO = dict(zip(_COMBO_VOWELS, range(0xE030, 0xE036)))
_CONN_COMBO = dict(zip(_COMBO_VOWELS, range(0xE038, 0xE03E)))

# The console READOUT does not report the codepoint the program painted; it
# resolves the drawn glyph through the FONT's unicode table and labels the slot
# (tools/host_cells.py label(): prefer a printable non-private-use codepoint,
# then a printable one, then the alias order).  The font's plain-mark slots
# carry the LOGICAL mark as an alias - out/console-16x32.psfu slot 500 is
# [U+064B, U+E000] - so a plain-mark cell reads as U+064B, while the connector
# slots (0..8 = [U+0001..U+0008, U+E010..]) and the combo slots have no
# printable alias and read as U+E01x / U+000B(+1) style.  That surface is what
# the smoke's delta carries, so the predicate normalizes BOTH the model and the
# readout text through the font's own table - the same rule host_cells applies,
# re-derived here rather than hardcoded, so a font change cannot silently
# invalidate it.  (Two codepoints sharing a slot are one glyph: U+064B and
# U+E000 are indistinguishable on the console, so no observable distinction is
# lost by normalizing.)
_FONT = Path(__file__).resolve().parent.parent / "out" / "console-16x32.psfu"


def _font_slots():
    data = _FONT.read_bytes()
    count = struct.unpack_from("<I", data, 16)[0]
    bytes_per_glyph = struct.unpack_from("<I", data, 20)[0]
    header = struct.unpack_from("<I", data, 8)[0]
    slots, i = {}, 0
    for block in data[header + count * bytes_per_glyph:].split(b"\xff"):
        if block:
            for q in block.split(b"\xfe"):
                if q:
                    try:
                        slots.setdefault(i, []).append(ord(q.decode()))
                    except Exception:
                        pass
            i += 1
    return slots


_SLOTS = _font_slots()
_SLOT_OF = {cp: slot for slot, cps in _SLOTS.items() for cp in cps}


def readout_cp(cp):
    """The codepoint the console readout reports for a cell painted as `cp`."""
    cps = _SLOTS.get(_SLOT_OF.get(cp, -1))
    if not cps:
        return cp
    printable = [c for c in cps if c >= 0x20]
    real = [c for c in printable if not (0xE000 <= c <= 0xF8FF)]
    return (real or printable or cps)[0]


def _joins_forward(joined, li):
    """True when the non-transparent character before logical index `li` is an
    initial/medial (forward-joining) letter - the connector condition."""
    prior = next((pos for _, pos in reversed(joined[:li])
                  if pos != "transparent"), None)
    return prior in ("init", "medi")


def _mark_cell(cp, joined, li):
    """A mark's painted cell: connector after a forward-joining letter."""
    return (CONNECTOR_MARKS if _joins_forward(joined, li) else DRAW_MARKS)[cp]


def _word_cells(word):
    """One word's cells, in the console's visual order: Arabic runs reversed,
    ASCII runs kept forward (ar_type.c:339-346), marks resolved to their
    painted cells, a shadda+vowel pair collapsed to its stacked cell."""
    joined = join_word(word)
    visual = shape(word)
    raw = [ord(ch) for ch in word]
    n = len(joined)
    cps = {}
    for vi, li in enumerate(range(n - 1, -1, -1)):
        cps[li] = visual[vi]
    for li in list(cps):
        if cps[li] in DRAW_MARKS:
            cps[li] = _mark_cell(cps[li], joined, li)
    # Task 54: the vowel's cell belongs to the shadda immediately before it.
    for li in range(1, n):
        if raw[li] in _COMBO_VOWELS and raw[li - 1] == _SHADDA:
            del cps[li]
    for li in range(n - 1):
        if raw[li] == _SHADDA and raw[li + 1] in _COMBO_VOWELS:
            combo = _CONN_COMBO if _joins_forward(joined, li) else _PLAIN_COMBO
            cps[li] = combo[raw[li + 1]]
    order = []
    end = n
    while end:
        start = end
        ascii_run = len(joined[end - 1][0]) == 1 and ord(joined[end - 1][0]) < 0x80
        while start and (len(joined[start - 1][0]) == 1
                         and ord(joined[start - 1][0]) < 0x80) == ascii_run:
            start -= 1
        order.extend(range(start, end) if ascii_run
                     else range(end - 1, start - 1, -1))
        end = start
    return [cps[li] for li in order]


def full_cells(text):
    """The console cell list for one logical row, spaces included (the shaper's
    own visual order, with marks resolved to their painted cells and every cell
    then normalized to what the readout reports - see readout_cp)."""
    out = []
    for wi, word in enumerate(reversed(text.split(" "))):
        if wi:
            out.append(0x20)
        out.extend(_word_cells(word))
    return [readout_cp(cp) for cp in out]


def inked(cells):
    return [c for c in cells if c != 0x20]


def expected_row(text):
    return inked(full_cells(text))


def parse_rows(text):
    """host_cells.py --rows text -> {row_label: [(col, cp), ...]}.

    Tokens are 'COL:U+XXXX' or 'COL:U+XXXX(+N)' for a cell that also carries N
    stacked marks (the readout's own notation); the first codepoint is the cell.
    """
    rows = {}
    for line in text.splitlines():
        m = re.match(r"^row\s+(\d+)\s*:(.*)$", line)
        if not m:
            continue
        cells = []
        for tok in m.group(2).split():
            tm = re.match(r"^(\d+):U\+([0-9A-Fa-f]+)", tok)
            if tm:
                cells.append((int(tm.group(1)), readout_cp(int(tm.group(2), 16))))
        rows[m.group(1)] = cells
    return rows


def check(entries, rows_text):
    """-> (present, missing, row_of) with row_of mapping entry -> row label."""
    rows = parse_rows(rows_text)
    present, missing, row_of = [], [], {}
    for entry in entries:
        want = expected_row(entry)
        hit = None
        for label, cells in rows.items():
            if [cp for _, cp in cells] == want:
                hit = label
                break
        if hit is None:
            missing.append(entry)
        else:
            present.append(entry)
            row_of[entry] = hit
    return present, missing, row_of


def read_entries(path):
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entries", required=True, help="one logical entry row per line")
    ap.add_argument("--rows", required=True, help="host_cells.py --rows text to test")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    args = ap.parse_args()
    entries = read_entries(args.entries)
    rows_text = Path(args.rows).read_text(encoding="utf-8")
    present, missing, _ = check(entries, rows_text)
    if not args.quiet:
        for e in present:
            print(f"  PASS  {e}")
        for e in missing:
            print(f"  FAIL  {e}")
    print(f"HELP: {len(entries)} entries, {len(present)} present, {len(missing)} missing")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
